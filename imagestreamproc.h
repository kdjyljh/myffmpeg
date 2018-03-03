#ifndef IMAGESTREAMPROC_H
#define IMAGESTREAMPROC_H

#include "streaminfodef.h"

#include <inttypes.h>
#include <cmath>
#include <limits.h>
#include <stdint.h>
#include <assert.h>
#include <string>
#include <boost/thread.hpp>
#include <deque>

using std::isnan;

class ImageStreamProc
{
public:
    explicit ImageStreamProc();
    void run();
    static void popImage(AVFrame &image);

private:
    bool streamOpen();
    void streamClose();
    void eventLoop();
    int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
    int packet_queue_init(PacketQueue *q);
    int read_thread();
    void stream_component_close(VideoState *is, int stream_index);
    void decoder_destroy(Decoder *d);
    void decoder_abort(Decoder *d, FrameQueue *fq);
    void packet_queue_abort(PacketQueue *q);
    void frame_queue_signal(FrameQueue *f);
    void packet_queue_flush(PacketQueue *q);
    void init_clock(Clock *c, int *queue_serial);
    void set_clock(Clock *c, double pts, int serial);
    void set_clock_at(Clock *c, double pts, int serial, double time);
    void packet_queue_destroy(PacketQueue *q);
    void frame_queue_destory(FrameQueue *f);
    void frame_queue_unref_item(Frame *vp);
    int is_realtime(AVFormatContext *s);
    void set_default_window_size(int width, int height, AVRational sar);
    void calculate_display_rect(Rect *rect,
                                       int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                       int pic_width, int pic_height, AVRational pic_sar);
    int stream_component_open(VideoState *is, int stream_index);
    int packet_queue_put(PacketQueue *q, AVPacket *pkt);
    int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);
    void step_to_next_frame(VideoState *is);
    void stream_toggle_pause(VideoState *is);
    int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
    void do_exit();
    int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);
    int frame_queue_nb_remaining(FrameQueue *f);
    void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes);
    double get_clock(Clock *c);
    int64_t get_valid_channel_layout(int64_t channel_layout, int channels);
    int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format);
    int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                     AVFilterContext *source_ctx, AVFilterContext *sink_ctx);
    int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params);
    void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, boost::condition_variable *empty_queue_cond);
    int decoder_start(Decoder *d, int (ImageStreamProc::*fn)(void *), void *arg);
    void packet_queue_start(PacketQueue *q);
    int video_thread(void *arg);
    int get_video_frame(VideoState *is, AVFrame *frame);
    int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
    int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);
    int get_master_sync_type(VideoState *is);
    double get_master_clock(VideoState *is);
    int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame);
    int audio_thread(void *arg);
    int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                       enum AVSampleFormat fmt2, int64_t channel_count2);
    Frame *frame_queue_peek_writable(FrameQueue *f);
    void frame_queue_push(FrameQueue *f);
    int subtitle_thread(void *arg);
    int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial);
    void video_refresh(void *opaque, double *remaining_time);
    void check_external_clock_speed(VideoState *is);
    void set_clock_speed(Clock *c, double speed);
    void video_display(VideoState *is);
    int video_open(VideoState *is);
    void video_audio_display(VideoState *s);
    int compute_mod(int a, int b);
    void video_image_display(VideoState *is);
    Frame *frame_queue_peek_last(FrameQueue *f);
    Frame *frame_queue_peek(FrameQueue *f);
    int upload_image(AVFrame *frame, struct SwsContext **img_convert_ctx);
    void frame_queue_next(FrameQueue *f);
    double vp_duration(VideoState *is, Frame *vp, Frame *nextvp);
    double compute_target_delay(double delay, VideoState *is);
    void update_video_pts(VideoState *is, double pts, int64_t pos, int serial);
    void sync_clock_to_slave(Clock *c, Clock *slave);
    Frame *frame_queue_peek_next(FrameQueue *f);

private:
    static void pushImage(AVFrame image);

private:
    /* options specified by the user */
    AVInputFormat *file_iformat;
    const char *input_filename;
    const char *window_title = nullptr;
    int default_width  = 640;
    int default_height = 480;
    int screen_width  = 0;
    int screen_height = 0;
    int audio_disable = 1;//由于audio_open函数没有实现，暂时不使用音频
    int video_disable = 0;
    int subtitle_disable;
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
    int seek_by_bytes = -1;
    int display_disable = 0;
    int borderless;
    int startup_volume = 100;
    int show_status = 1;
    int av_sync_type = AV_SYNC_AUDIO_MASTER;
    int64_t start_time = AV_NOPTS_VALUE;
    int64_t duration = AV_NOPTS_VALUE;
    int fast = 0;
    int genpts = 0;
    int lowres = 0;
    int decoder_reorder_pts = -1;
    int autoexit;
    int exit_on_keydown;
    int exit_on_mousedown;
    int loop = 1;
    int framedrop = -1;
    int infinite_buffer = -1;
    enum ShowMode show_mode = SHOW_MODE_NONE;
    const char *audio_codec_name = nullptr;
    const char *subtitle_codec_name = nullptr;
    const char *video_codec_name = nullptr;
    double rdftspeed = 0.02;
    int64_t cursor_last_shown;
//    int cursor_hidden = 0;
    #if CONFIG_AVFILTER
    const char **vfilters_list = NULL;
    int nb_vfilters = 0;
    char *afilters = nullptr;
    #endif
    int autorotate = 1;
    int find_stream_info = 1;

    /* current context */
     int is_full_screen;
    int64_t audio_callback_time;

    AVPacket flush_pkt;

private:
    VideoState *is;
    boost::thread readThread;
    boost::thread videoDecoderThread;
    boost::thread audioDecoderThread;
    boost::thread subtitleDecoderThread;
    static std::deque<AVFrame> imageQueue;
};

#endif // IMAGESTREAMPROC_H
