#include "imageshow.h"
#include <QApplication>
#include <QDesktopWidget>

ImageShow::ImageShow(QWidget *parent) : QWidget(parent)
{
    viewLable = new QLabel(this);
    QPoint centerPoint;
    QRect screenRect = QApplication::desktop()->screenGeometry();
    centerPoint.setX(screenRect.width() / 2 - width() / 2);
    centerPoint.setY(screenRect.height() / 2 - height() / 2);
    setGeometry(QRect(centerPoint, QSize(800, 500)));
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
