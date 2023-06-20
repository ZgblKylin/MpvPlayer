#ifndef MPV_PLAYER_HPP
#define MPV_PLAYER_HPP

#include <QtCore/QtCore>
#include <QtGui/QtGui>
#include <QtWidgets/QtWidgets>
#include <QtQml/QtQml>
#include <QtQuick/QtQuick>

struct mpv_handle;
class MpvPlayer {
 public:
  virtual ~MpvPlayer();

  QString name() const;
  void setName(const QString& name);
  virtual void nameChanged(const QString& name) = 0;

  void setUrl(const QUrl& url);
  QUrl url() const;
  virtual void urlChanged(const QUrl& url) = 0;

  struct mpv_handle* mpv_handle() const;

  void play(const QUrl& url = QUrl());
  void pause();
  bool isPaused() const;
  void setPaused(bool paused);
  virtual void pausedChanged(bool paused) = 0;
  void resume();
  void stop();

  enum PlayState { Play, Pause, EndReached, Stop, Unknown };
  virtual void playStateChanged(int state) = 0;
  virtual void durationChanged(double value) = 0;
  virtual void videoSizeChanged(int width, int height) = 0;
  virtual void videoStarted() = 0;
  virtual void newLogMessage(QtMsgType level, const QString& prefix,
                             const QString& msg) = 0;

  QSize videoSize() const;
  QSize displaySize() const;
  void setCropVideo(const QRect& rect);
  void setCropVideo(const QRectF& rect_ratio);
  void uncropVideo();

  template <typename... Args>
  QVariant playerCommand(Args&&... args);
  virtual QVariant command(const QVariant& args);

  virtual bool setPlayerProperty(const QString& name, const QVariant& value);
  template <typename T>
  T getPlayerProperty(const QString& name) const;

 protected:
  void processQEvent(QEvent* event);
  void processMpvEvents();

 private:
  friend class MpvPlayerWidget;
  friend class MpvPlayerOpenGLWidget;
  friend class MpvPlayerQuickObject;
  explicit MpvPlayer(QObject* impl, const QString& name = "");

  QVariant getPlayerProperty_(const QString& name) const;

  struct Private;
  QScopedPointer<Private> d;
};

class MpvPlayerWidget : public QWidget, public MpvPlayer {
  Q_OBJECT
  Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
  Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
  Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged)

 public:
  MpvPlayerWidget(const QString& name = "", QWidget* parent = nullptr,
                  Qt::WindowFlags f = Qt::WindowFlags());

  Q_SIGNAL void nameChanged(const QString& name) override;
  Q_SIGNAL void urlChanged(const QUrl& url) override;
  Q_SIGNAL void pausedChanged(bool paused) override;
  Q_SIGNAL void playStateChanged(int state) override;
  Q_SIGNAL void durationChanged(double value) override;
  Q_SIGNAL void videoSizeChanged(int width, int height) override;
  Q_SIGNAL void videoStarted() override;
  Q_SIGNAL void newLogMessage(QtMsgType level, const QString& prefix,
                              const QString& msg) override;

  Q_SLOT QVariant command(const QVariant& args) override {
    return MpvPlayer::command(args);
  }
  Q_SLOT bool setPlayerProperty(const QString& name,
                                const QVariant& value) override {
    return MpvPlayer::setPlayerProperty(name, value);
  }

 protected:
  bool event(QEvent* event) override;

 private:
  friend class MpvPlayer;
  MpvPlayer::Private* d;
};

class MpvPlayerOpenGLWidget : public QOpenGLWidget, public MpvPlayer {
  Q_OBJECT
  Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
  Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
  Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged)

 public:
  MpvPlayerOpenGLWidget(const QString& name = "", QWidget* parent = nullptr,
                        Qt::WindowFlags f = Qt::WindowFlags());
  ~MpvPlayerOpenGLWidget() override;

  Q_SIGNAL void nameChanged(const QString& name) override;
  Q_SIGNAL void urlChanged(const QUrl& url) override;
  Q_SIGNAL void pausedChanged(bool paused) override;
  Q_SIGNAL void playStateChanged(int state) override;
  Q_SIGNAL void durationChanged(double value) override;
  Q_SIGNAL void videoSizeChanged(int width, int height) override;
  Q_SIGNAL void videoStarted() override;
  Q_SIGNAL void newLogMessage(QtMsgType level, const QString& prefix,
                              const QString& msg) override;

  Q_SLOT QVariant command(const QVariant& args) override {
    return MpvPlayer::command(args);
  }
  Q_SLOT bool setPlayerProperty(const QString& name,
                                const QVariant& value) override {
    return MpvPlayer::setPlayerProperty(name, value);
  }

 protected:
  bool event(QEvent* event) override;
  void initializeGL() override;
  void paintGL() override;

 private:
  friend class MpvPlayer;
  static void* get_proc_address(void* ctx, const char* name);
  static void on_update(void* ctx);
  void maybeUpdate();
  MpvPlayer::Private* d;
};

// qmlRegisterType<MpvPlayerQuickObject>("MpvPlayer", 1, 0,
//                                         "MpvPlayerQuickObject");
class MpvPlayerQuickObject : public QQuickFramebufferObject, public MpvPlayer {
  Q_OBJECT
  Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
  Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
  Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged)

 public:
  MpvPlayerQuickObject(const QString& name = "", QQuickItem* parent = 0);
  ~MpvPlayerQuickObject() override;

  Renderer* createRenderer() const override;

  Q_SIGNAL void nameChanged(const QString& name) override;
  Q_SIGNAL void urlChanged(const QUrl& url) override;
  Q_SIGNAL void pausedChanged(bool paused) override;
  Q_SIGNAL void playStateChanged(int state) override;
  Q_SIGNAL void durationChanged(double value) override;
  Q_SIGNAL void videoSizeChanged(int width, int height) override;
  Q_SIGNAL void videoStarted() override;
  Q_SIGNAL void newLogMessage(QtMsgType level, const QString& prefix,
                              const QString& msg) override;

  Q_SLOT QVariant command(const QVariant& args) override {
    return MpvPlayer::command(args);
  }
  Q_SLOT bool setPlayerProperty(const QString& name,
                                const QVariant& value) override {
    return MpvPlayer::setPlayerProperty(name, value);
  }

 private:
  friend class MpvPlayer;
  struct MpvQuickRenderer;
  static void* get_proc_address(void* ctx, const char* name);
  MpvPlayer::Private* d;
};

namespace {
template <typename T>
inline QVariantList& pack_args(QVariantList& params, T&& arg) {
  params << arg;
  return params;
}

template <typename T, typename... Args>
inline QVariantList& pack_args(QVariantList& params, T&& arg, Args&&... args) {
  params << QVariant(std::forward<T>(arg));
  return pack_args(params, std::forward<Args>(args)...);
}
}  // namespace

template <typename... Args>
inline QVariant MpvPlayer::playerCommand(Args&&... args) {
  QVariantList params;
  return command(pack_args(params, std::forward<Args>(args)...));
}

template <typename T>
inline T MpvPlayer::getPlayerProperty(const QString& name) const {
  return getPlayerProperty_(name).value<T>();
}

#endif  // MPV_PLAYER_HPP
