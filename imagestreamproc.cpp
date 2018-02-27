#include "imagestreamproc.h"

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = static_cast<VideoState*>(ctx);
    return is->abort_request;
}

static boost::mutex wait_mutex;
ImageStreamProc::ImageStreamProc() :
    input_filename("rtsp://192.168.0.1/livestream/12"),
    file_iformat(nullptr),
    is(nullptr)
{
    av_log_set_flags(AV_LOG_PRINT_LEVEL);
    avdevice_register_all();
    avfilter_register_all();
    av_register_all();
    avformat_network_init();

    init_opts();

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    is = static_cast<VideoState*>(av_mallocz(sizeof(VideoState)));

    streamOpen();
}

bool ImageStreamProc::streamOpen()
{
    if (nullptr == is) {
        return false;
    }

    is->filename = av_strdup(input_filename);
    is->iformat = file_iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    is->continue_read_thread = new boost::condition_variable;

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    if (startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    startup_volume = av_clip(startup_volume, 0, 100);
    is->audio_volume = startup_volume;
    is->muted = 0;
    is->av_sync_type = av_sync_type;
    readThread = boost::thread(&ImageStreamProc::read_thread, this);
    return true;
fail:
    streamClose();
    return false;
}

void ImageStreamProc::streamClose()
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    readThread.join();

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    delete is->continue_read_thread;
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    av_free(is);
}

int ImageStreamProc::frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    f->mutex = new boost::mutex;
    f->cond = new boost::condition_variable;
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

/* packet queue handling */
int ImageStreamProc::packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = new boost::mutex;
    q->cond = new boost::condition_variable;
    q->abort_request = 1;
    return 0;
}

int ImageStreamProc::read_thread()
{
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;

    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    if (find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
        int orig_nb_streams = ic->nb_streams;

        err = avformat_find_stream_info(ic, opts);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (seek_by_bytes < 0)
        seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i], av_get_media_type_string((AVMediaType)i));
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms */
            {
                boost::unique_lock<boost::mutex> lock(wait_mutex);
                is->continue_read_thread->wait_for(lock, boost::chrono::milliseconds(10));
            }
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error)
                break;
            {
                boost::unique_lock<boost::mutex> lock(wait_mutex);
                is->continue_read_thread->wait_for(lock, boost::chrono::milliseconds(10));
            }
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (ret != 0) {
        do_exit();
    }
    return 0;
}

void ImageStreamProc::stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

void ImageStreamProc::decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    decoderThread.join();
    packet_queue_flush(d->queue);
}

void ImageStreamProc::decoder_destroy(Decoder *d)
{
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

void ImageStreamProc::packet_queue_abort(PacketQueue *q)
{
    boost::unique_lock<boost::mutex> lock(*q->mutex);
    q->cond->notify_one();
    q->abort_request = 1;
}

void ImageStreamProc::frame_queue_signal(FrameQueue *f)
{
    boost::unique_lock<boost::mutex> lock(*f->mutex);
    f->cond->notify_one();
}

void ImageStreamProc::packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    boost::unique_lock<boost::mutex> lock(*q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
}

void ImageStreamProc::init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

void ImageStreamProc::set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

void ImageStreamProc::set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void ImageStreamProc::packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    delete q->mutex;
    delete q->cond;
}

void ImageStreamProc::frame_queue_destory(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    delete f->mutex;
    delete f->cond;
}

void ImageStreamProc::frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

int ImageStreamProc::is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                 || !strncmp(s->url, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

void ImageStreamProc::set_default_window_size(int width, int height, AVRational sar)
{
    Rect rect;
    calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}

void ImageStreamProc::calculate_display_rect(Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    float aspect_ratio;
    int width, height, x, y;

    if (pic_sar.num == 0)
        aspect_ratio = 0;
    else
        aspect_ratio = av_q2d(pic_sar);

    if (aspect_ratio <= 0.0)
        aspect_ratio = 1.0;
    aspect_ratio *= (float)pic_width / (float)pic_height;

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = lrint(height * aspect_ratio) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = lrint(width / aspect_ratio) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX(width,  1);
    rect->h = FFMAX(height, 1);
}

/* open a given stream. Return 0 if OK */
int ImageStreamProc::stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    video_codec_name; break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                goto fail;
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            nb_channels    = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output */
        if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, &ImageStreamProc::audio_thread, is)) < 0)
            goto out;
//        SDL_PauseAudioDevice(audio_dev, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
        if ((ret = decoder_start(&is->viddec, &ImageStreamProc::video_thread, is)) < 0)
            goto out;
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, &ImageStreamProc::subtitle_thread, is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

int ImageStreamProc::packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    {
        boost::unique_lock<boost::mutex> lock(*q->mutex);
        ret = packet_queue_put_private(q, pkt);
    }

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

int ImageStreamProc::packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

    pkt1 = (MyAVPacketList*)av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    q->cond->notify_one();
    return 0;
}

void ImageStreamProc::step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

/* pause or resume the video */
void ImageStreamProc::stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

int ImageStreamProc::packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

void ImageStreamProc::do_exit()
{
    if (is) {
        streamClose();
    }
    uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

int ImageStreamProc::stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

/* return the number of undisplayed frames in the queue */
int ImageStreamProc::frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* seek in the stream */
void ImageStreamProc::stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        boost::unique_lock<boost::mutex> lock(wait_mutex);
        is->continue_read_thread->notify_one();
    }
}

double ImageStreamProc::get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

int64_t ImageStreamProc::get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

int ImageStreamProc::configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }


    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}

int ImageStreamProc::configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

int ImageStreamProc::audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    return 1;
//    SDL_AudioSpec wanted_spec, spec;
//    const char *env;
//    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
//    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
//    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

//    env = SDL_getenv("SDL_AUDIO_CHANNELS");
//    if (env) {
//        wanted_nb_channels = atoi(env);
//        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
//    }
//    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
//        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
//        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
//    }
//    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
//    wanted_spec.channels = wanted_nb_channels;
//    wanted_spec.freq = wanted_sample_rate;
//    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
//        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
//        return -1;
//    }
//    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
//        next_sample_rate_idx--;
//    wanted_spec.format = AUDIO_S16SYS;
//    wanted_spec.silence = 0;
//    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
//    wanted_spec.callback = sdl_audio_callback;
//    wanted_spec.userdata = opaque;
//    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
//        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
//               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
//        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
//        if (!wanted_spec.channels) {
//            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
//            wanted_spec.channels = wanted_nb_channels;
//            if (!wanted_spec.freq) {
//                av_log(NULL, AV_LOG_ERROR,
//                       "No more combinations to try, audio open failed\n");
//                return -1;
//            }
//        }
//        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
//    }
//    if (spec.format != AUDIO_S16SYS) {
//        av_log(NULL, AV_LOG_ERROR,
//               "SDL advised audio format %d is not supported!\n", spec.format);
//        return -1;
//    }
//    if (spec.channels != wanted_spec.channels) {
//        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
//        if (!wanted_channel_layout) {
//            av_log(NULL, AV_LOG_ERROR,
//                   "SDL advised channel count %d is not supported!\n", spec.channels);
//            return -1;
//        }
//    }

//    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
//    audio_hw_params->freq = spec.freq;
//    audio_hw_params->channel_layout = wanted_channel_layout;
//    audio_hw_params->channels =  spec.channels;
//    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
//    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
//    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
//        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
//        return -1;
//    }
//    return spec.size;
}

void ImageStreamProc::decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, boost::condition_variable *empty_queue_cond)
{
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

int ImageStreamProc::decoder_start(Decoder *d, int (ImageStreamProc::*fn)(void *), void *arg)
{
    packet_queue_start(d->queue);
    decoderThread = boost::thread(fn, this, arg);
    return 0;
}

void ImageStreamProc::packet_queue_start(PacketQueue *q)
{
    boost::unique_lock<boost::mutex> lock(*q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
}

int ImageStreamProc::video_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = (AVPixelFormat)-2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    if (!graph) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

#endif

    if (!frame) {
#if CONFIG_AVFILTER
        avfilter_graph_free(&graph);
#endif
        return AVERROR(ENOMEM);
    }

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name((AVPixelFormat)frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                do_exit();
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = (AVPixelFormat)frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
        }
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

int ImageStreamProc::get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop>0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

int ImageStreamProc::decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0) {
                boost::unique_lock<boost::mutex> lock(wait_mutex);
                d->empty_queue_cond->notify_one();
            }
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
            }
        } while (d->queue->serial != d->pkt_serial);

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                       d->packet_pending = 1;
                       av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int ImageStreamProc::packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    {
        boost::unique_lock<boost::mutex> lock(*q->mutex);
        for (;;) {
            if (q->abort_request) {
                ret = -1;
                break;
            }

            pkt1 = q->first_pkt;
            if (pkt1) {
                q->first_pkt = pkt1->next;
                if (!q->first_pkt)
                    q->last_pkt = NULL;
                q->nb_packets--;
                q->size -= pkt1->pkt.size + sizeof(*pkt1);
                q->duration -= pkt1->pkt.duration;
                *pkt = pkt1->pkt;
                if (serial)
                    *serial = pkt1->serial;
                av_free(pkt1);
                ret = 1;
                break;
            } else if (!block) {
                ret = 0;
                break;
            } else {
                q->cond->wait(lock);
            }
        }
    }
    return ret;
}

int ImageStreamProc::get_master_sync_type(VideoState *is)
{
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
double ImageStreamProc::get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

int ImageStreamProc::configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
//    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    enum AVPixelFormat pix_fmts[2];
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 1;
    int i, j;

//    for (i = 0; i < renderer_info.num_texture_formats; i++) {
//        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
//            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
//                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
//                break;
//            }
//        }
//    }

    pix_fmts[0] = AV_PIX_FMT_YUV420P;
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (autorotate) {
        double theta  = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);
            INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

int ImageStreamProc::audio_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};

#if CONFIG_AVFILTER
                dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   (AVSampleFormat)frame->format, frame->channels)    ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial               != last_serial;

                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                    av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->channels, av_get_sample_fmt_name((AVSampleFormat)frame->format), buf2, is->auddec.pkt_serial);

                    is->audio_filter_src.fmt            = (AVSampleFormat)frame->format;
                    is->audio_filter_src.channels       = frame->channels;
                    is->audio_filter_src.channel_layout = dec_channel_layout;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;

                    if ((ret = configure_audio_filters(is, afilters, 1)) < 0)
                        goto the_end;
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

int ImageStreamProc::cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

Frame *ImageStreamProc::frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    {
        boost::unique_lock<boost::mutex> lock(*f->mutex);
        while (f->size >= f->max_size &&
               !f->pktq->abort_request) {
            f->cond->wait(lock);
        }
    }

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

void ImageStreamProc::frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    boost::unique_lock<boost::mutex> lock(*f->mutex);
    f->size++;
    f->cond->notify_one();
}

int ImageStreamProc::subtitle_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

int ImageStreamProc::queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}
