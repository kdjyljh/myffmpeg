#include "imageshow.h"
#include <QApplication>
#include <QDesktopWidget>

ImageShow::ImageShow(QWidget *parent) :
    QWidget(parent),
    imgSP(boost::shared_ptr<ImageStreamProc>(new ImageStreamProc))
{
    viewLable = new QLabel(this);
    QPoint centerPoint;
    QRect screenRect = QApplication::desktop()->screenGeometry();
    centerPoint.setX(screenRect.width() / 2 - width() / 2);
    centerPoint.setY(screenRect.height() / 2 - height() / 2);
    setGeometry(QRect(centerPoint, QSize(800, 500)));
    boost::thread t(&ImageStreamProc::run, imgSP);
    boost::thread tt([&]() {
        while (true) {
            AVFrame frame;
            ImageStreamProc::popImage(frame);
            QImage image(frame.data[0], frame.width, frame.height, QImage::Format_RGB888);
            setLabelPix(image);
//            boost::this_thread::sleep_for(boost::chrono::milliseconds(1));
        }
    });
}

boost::shared_ptr<ImageShow> ImageShow::getInstace()
{
    static boost::shared_ptr<ImageShow> instance(new ImageShow);
    return instance;
}

void ImageShow::setLabelPix(const QImage image)
{
    QPixmap pix = QPixmap::fromImage(image);

    viewLable->setGeometry((size().width() - pix.width()) / 2, (size().height() - pix.height()) / 2,
                           pix.width(), pix.height());
    viewLable->setPixmap(pix);
}
