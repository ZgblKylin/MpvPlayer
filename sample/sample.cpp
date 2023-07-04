#include "sample.hpp"
#include <cstdlib>
#include <clocale>
#include <QtWidgets/QtWidgets>
#include <QtQuickWidgets/QtQuickWidgets>
#include <MpvPlayer.hpp>

MpvPlayerQuickInput::MpvPlayerQuickInput(const QString& name, const QUrl& url,
                                         bool paused)
    : name_(name), url_(url), paused_(paused) {}

QString MpvPlayerQuickInput::name() const { return name_; }

void MpvPlayerQuickInput::setName(const QString& name) {
  if (name != name_) {
    name_ = name;
    emit nameChanged(name);
  }
}

QUrl MpvPlayerQuickInput::url() const { return url_; }

void MpvPlayerQuickInput::setUrl(const QUrl& url) {
  if (url != url_) {
    url_ = url;
    emit urlChanged(url);
  }
}

bool MpvPlayerQuickInput::isPaused() const { return paused_; }

void MpvPlayerQuickInput::setPaused(bool paused) {
  if (paused != paused_) {
    paused_ = paused;
    emit pausedChanged(paused);
  }
}

int main(int argc, char* argv[]) {
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
  QApplication app(argc, argv);

  // Qt sets the locale in the QGuiApplication constructor, but libmpv
  // requires the LC_NUMERIC category to be set to "C", so change it back.
  std::setlocale(LC_NUMERIC, "C");

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption(QCommandLineOption(QStringList() << "o"
                                                    << "opengl-window",
                                      "Use opengl for root window"));
  parser.addOption(QCommandLineOption(
      QStringList() << "t"
                    << "type",
      "Type of player, could be widget/opengl/qml", "type", "widget"));
  parser.addOption(QCommandLineOption(
      QStringList() << "r"
                    << "repeat",
      "Repeat count of first video for multiple players", "repeat", "1"));
  parser.addOption(
      QCommandLineOption(QStringList() << "s"
                                       << "split",
                         "Used with --repeat, split first video into n count"));
  parser.addOption(QCommandLineOption(QStringList() << "m"
                                                    << "mute",
                                      "Mute all players"));
  parser.addOption(QCommandLineOption(
      QStringList() << "p"
                    << "performance-mode",
      "Performance mode, disable some features to improve performance"));
  parser.addPositionalArgument("url", "Video urls", "urls...");
  parser.process(app);

  bool is_opengl_window = parser.isSet("opengl-window");
  bool is_widget = parser.value("type").toLower() == "widget";
  bool is_opengl = parser.value("type").toLower() == "opengl";
  bool is_qml = parser.value("type").toLower() == "qml";
  bool is_mute = parser.isSet("mute");
  bool is_performance_mode = parser.isSet("performance-mode");
  int count = std::max(parser.value("repeat").toInt(), 1);
  bool split = parser.isSet("split");
  QStringList urls = parser.positionalArguments();
  qDebug() << "is_opengl_window:" << is_opengl_window;
  qDebug() << "is_widget:" << is_widget;
  qDebug() << "is_opengl:" << is_opengl;
  qDebug() << "is_qml:" << is_qml;
  qDebug() << "count:" << count;
  qDebug() << "split:" << split;
  qDebug() << "urls:" << urls;
  if (count > 1) {
    QString url = urls.first();
    urls = QStringList();
    for (int i = 0; i < count; ++i) {
      urls << url;
    }
  }
  if (split) {
    count = std::pow(std::ceil(std::sqrt(count)), 2);
  }

  if ((!is_widget && !is_opengl && !is_qml) || urls.isEmpty()) {
    parser.showHelp(EXIT_FAILURE);
  }

  qmlRegisterType<MpvPlayerQuickObject>("MpvPlayer", 1, 0,
                                        "MpvPlayerQuickObject");

  QWidget* window;
  QGridLayout* layout;
  int width = std::ceil(std::sqrt(count));
  int height = std::ceil(count / width);
  if (is_qml) {
    QQuickWidget* widget = new QQuickWidget;
    widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    widget->rootContext()->setContextProperty(
        "players", QVariant::fromValue(QList<QObject*>()));
    widget->setSource(QUrl("qrc:///MpvPlayerSample/sample.qml"));
    window = widget;
  } else {
    if (is_opengl_window) {
      window = new QOpenGLWidget;
    } else {
      window = new QWidget;
    }
    layout = new QGridLayout;
    window->setLayout(layout);
  }

  QVector<MpvPlayer*> players;
  for (int i = 0; i < urls.size(); ++i) {
    MpvPlayer* player;
    if (!is_qml) {
      if (is_widget) {
        player = new MpvPlayerWidget(QString::number(i));
      } else if (is_opengl) {
        player = new MpvPlayerOpenGLWidget(QString::number(i));
      }
      if (is_mute) {
        player->disableAudio();
      }
      if (is_performance_mode) {
        player->enableHighPerformanceMode();
      }
      layout->addWidget(dynamic_cast<QWidget*>(player), i / width, i % width);
    }
    players << player;
  }

  window->resize(1280, 720);
  window->show();
  app.processEvents();

  if (!is_qml) {
    for (int i = 0; i < urls.size(); ++i) {
      if (QFile::exists(urls[i])) {
        players[i]->play(QUrl::fromLocalFile(urls[i]));
      } else {
        players[i]->play(urls[i]);
      }
      if (split) {
        int row = i / width;
        int col = i % width;
        players[i]->setCropVideo(QRectF(col * 1.0 / width, row * 1.0 / height,
                                        1.0 / width, 1.0 / height));
      }
    }
  } else {
    QList<QObject*> playerList;
    for (int i = 0; i < urls.size(); ++i) {
      QUrl url;
      if (QFile::exists(urls[i])) {
        url = QUrl::fromLocalFile(urls[i]);
      } else {
        url = urls[i];
      }
      playerList << new MpvPlayerQuickInput(QString::number(i), url, true);
    }
    QQuickWidget* widget = qobject_cast<QQuickWidget*>(window);
    widget->rootContext()->setContextProperty("players",
                                              QVariant::fromValue(playerList));
  }

  return app.exec();
}
