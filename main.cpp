#include <QApplication>
#include <QtCore>
#include <glog/logging.h>

#include "imageshow.h"

int main(int argc, char *argv[])
{
    google::InitGoogleLogging("myffmplay");
    FLAGS_log_dir = "/tmp/";
    FLAGS_logtostderr = 1;

    QCoreApplication::setAttribute(Qt::AA_X11InitThreads);

    QApplication a(argc, argv);

    ImageShow::getInstace()->show();

    return a.exec();
}
