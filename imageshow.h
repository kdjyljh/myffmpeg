#ifndef IMAGESHOW_H
#define IMAGESHOW_H

#include <QWidget>
#include <QLabel>
#include <boost/shared_ptr.hpp>
#include <deque>

#include "imagestreamproc.h"

class ImageShow : public QWidget
{
    Q_OBJECT
public:
    static boost::shared_ptr<ImageShow> getInstace();
    static void pushImage(QImage image);
    static QImage popImage();

signals:

public slots:

private:
    void setLabelPix(const QImage image);
    explicit ImageShow(QWidget *parent = nullptr);

private:
    QLabel *viewLable;
    boost::shared_ptr<ImageStreamProc> imgSP;
};

#endif // IMAGESHOW_H
