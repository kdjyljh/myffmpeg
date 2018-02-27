#ifndef IMAGESHOW_H
#define IMAGESHOW_H

#include <QWidget>
#include <QLabel>
#include <boost/shared_ptr.hpp>
#include <deque>

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

private:
    QLabel *viewLable;
    explicit ImageShow(QWidget *parent = nullptr);
};

#endif // IMAGESHOW_H
