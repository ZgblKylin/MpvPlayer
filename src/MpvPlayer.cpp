#include "MpvPlayer.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <mpv/render.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include "libmpv_qthelper.hpp"

#include <QtWidgets/QtWidgets>

Q_LOGGING_CATEGORY(MPV, "MPV",
                   QLibraryInfo::isDebugBuild() ? QtDebugMsg : QtInfoMsg)
#define MpvLog(name, log) log(MPV).nospace().noquote() << '[' << name << "] "

#define MpvDebug() MpvLog(d->name_, qCDebug)
#define MpvInfo() MpvLog(d->name_, qCInfo)
#define MpvWarning() MpvLog(d->name_, qCWarning)
#define MpvCritical() MpvLog(d->name_, qCCritical)
#define MpvFatal(format, ...)                                                \
  QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, \
                 MPV().categoryName())                                       \
      .fatal("[%s] " format, qPrintable(d->name_), ##__VA_ARGS__)

#define MpvPDebug() MpvLog(name_, qCDebug)
#define MpvPInfo() MpvLog(name_, qCInfo)
#define MpvPWarning() MpvLog(name_, qCWarning)
#define MpvPCritical() MpvLog(name_, qCCritical)
#define MpvPFatal(format, ...)                                               \
  QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, \
                 MPV().categoryName())                                       \
      .fatal("[%s] " format, qPrintable(name_), ##__VA_ARGS__)

namespace {
#define CHECK_MPV_ERROR(x)                             \
  do {                                                 \
    int ret = x;                                       \
    if (ret != MPV_ERROR_SUCCESS) {                    \
      MpvWarning() << "Error executing " << #x << ": " \
                   << mpv_error_string(ret);           \
    }                                                  \
  } while (0)

#define CHECK_MPV_ERROR_RET(ret, x)                    \
  do {                                                 \
    ret = x;                                           \
    if (ret != MPV_ERROR_SUCCESS) {                    \
      MpvWarning() << "Error executing " << #x << ": " \
                   << mpv_error_string(ret);           \
    }                                                  \
  } while (0)
}  // namespace

struct MpvPlayer::Private {
  Q_DISABLE_COPY(Private)

  MpvPlayer* q;
  explicit Private(MpvPlayer* q_ptr) : q(q_ptr) {}

  QObject* impl_ = nullptr;
  QString name_{};
  QUrl url_{};
  PlayState state_ = Stop;
  void changeState(PlayState state, bool resume = false);

  struct mpv_handle* mpv_ = nullptr;
  mpv_render_context* mpv_gl_ = nullptr;

  std::atomic_bool mpv_event_thread_running_ = ATOMIC_VAR_INIT(false);
  std::thread mpv_event_thread_{};
  void processMpvEvents();
};

void MpvPlayer::Private::changeState(PlayState state, bool resume) {
  if (state_ == state) {
    return;
  }

  switch (state) {
    case Stop:
      break;
    case Play:
      if (resume && (state_ != Pause)) {
        return;
      }
      break;
    case Pause:
      if (state_ != Play) {
        return;
      }
      break;
    case EndReached:
      break;
    default:
      break;
  }
  state_ = state;
  emit q->playStateChanged(state);
}

void MpvPlayer::Private::processMpvEvents() {
  // Process all events, until the event queue is empty.
  while (mpv_event_thread_running_.load(std::memory_order_acquire) && mpv_) {
    mpv_event* event = mpv_wait_event(mpv_, -1);
    if (event->event_id == MPV_EVENT_NONE) {
      continue;
    }

    switch (event->event_id) {
      case MPV_EVENT_START_FILE: {  /// 6: Notification before playback start of
                                    /// a file (before the file is loaded).* See
                                    /// also mpv_event and mpv_event_start_file.
        MpvPDebug() << "File start";
      } break;

      case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property* prop = (mpv_event_property*)event->data;
        QVariant value;
        switch (prop->format) {
          case MPV_FORMAT_STRING:
            value = QString::fromUtf8(*(char**)prop->data);
            break;
          case MPV_FORMAT_OSD_STRING:
            value = QString::fromUtf8(*(char**)prop->data);
            break;
          case MPV_FORMAT_FLAG:
            value = *(int*)prop->data;
            break;
          case MPV_FORMAT_INT64:
            value = *(int64_t*)prop->data;
            break;
          case MPV_FORMAT_DOUBLE:
            value = *(double*)prop->data;
            break;
          case MPV_FORMAT_NODE: {
            value = mpv::qt::node_to_variant((mpv_node*)prop->data);
          } break;
          default:
            break;
        }
        MpvPDebug() << "Property: " << prop->name << ' ' << value;
        if (strcmp(prop->name, "duration") == 0) {
          double time = value.value<double>();
          if (time > 0) {
            emit q->durationChanged(time);
          }
        } else if (strcmp(prop->name, "pause") == 0) {
          bool resume = value.value<bool>();
          changeState(resume ? Play : Pause, resume);
        } else if (strcmp(prop->name, "eof-reached") == 0) {
          if (value.value<bool>()) {
            changeState(EndReached);
          }
        }
      } break;

      case MPV_EVENT_FILE_LOADED: {
        emit q->videoStarted();
        changeState(Play);
        MpvPDebug() << "File loaded";
      } break;

      case MPV_EVENT_VIDEO_RECONFIG: {
        /**
         * Happens after video changed in some way. This can happen on
         * resolution changes, pixel format changes, or video filter changes.
         * The event is sent after the video filters and the VO are
         * reconfigured. Applications embedding a mpv window should listen to
         * this event in order to resize the window if needed. Note that this
         * event can happen sporadically, and you should check yourself whether
         * the video parameters really changed before doing something expensive.
         */
      } break;

      case MPV_EVENT_AUDIO_RECONFIG: {
        /**
         * Similar to MPV_EVENT_VIDEO_RECONFIG. This is relatively
         * uninteresting, because there is no such thing as audio output
         * embedding.
         */
      } break;

      case MPV_EVENT_LOG_MESSAGE: {
        auto* msg = static_cast<mpv_event_log_message*>(event->data);

        QtMsgType level;
        if (msg->log_level >= MPV_LOG_LEVEL_FATAL) {
          level = QtCriticalMsg;
        } else if (msg->log_level >= MPV_LOG_LEVEL_ERROR) {
          level = QtCriticalMsg;
        } else if (msg->log_level >= MPV_LOG_LEVEL_WARN) {
          level = QtWarningMsg;
        } else if (msg->log_level >= MPV_LOG_LEVEL_INFO) {
          level = QtInfoMsg;
        } else if (msg->log_level >= MPV_LOG_LEVEL_V) {
          level = QtDebugMsg;
        } else if (msg->log_level >= MPV_LOG_LEVEL_DEBUG) {
          level = QtDebugMsg;
          // continue;
        } else if (msg->log_level >= MPV_LOG_LEVEL_TRACE) {
          level = QtDebugMsg;
          // continue;
        } else {
          continue;
        }

        QString prefix = QString::fromUtf8(msg->prefix);

        // Trim text end
        QString text = QString::fromUtf8(msg->text);
        int length = text.length();
        for (int i = length - 1; i >= 0; --i) {
          if (text[i].isSpace()) {
            --length;
          } else {
            break;
          }
        }
        text = text.left(length);

        QString message = QStringLiteral("[%1] %2").arg(prefix, text);
        // switch (level) {
        //   case QtFatalMsg:
        //     // MpvPFatal("%s", qPrintable(message));
        //     MpvPCritical() << message;
        //     break;
        //   case QtCriticalMsg:
        //     MpvPCritical() << message;
        //     break;
        //   case QtWarningMsg:
        //     MpvPWarning() << message;
        //     break;
        //   case QtInfoMsg:
        //     MpvPInfo() << message;
        //     break;
        //   case QtDebugMsg:
        //     MpvPDebug() << message;
        //     break;
        //   default:
        //     continue;
        // }

        emit q->newLogMessage(level, prefix, text);
      } break;

      case MPV_EVENT_SHUTDOWN: {
        if (mpv_) {
          mpv_terminate_destroy(std::exchange(mpv_, nullptr));
        }
      } break;

      default:
        // Ignore uninteresting or unknown events.
        break;
    }
  }
  qDebug() << "Finish";
}

void MpvPlayer::nameChanged(const QString&) {}
void MpvPlayer::urlChanged(const QUrl&) {}
void MpvPlayer::pausedChanged(bool) {}
void MpvPlayer::playStateChanged(int) {}
void MpvPlayer::durationChanged(double) {}
void MpvPlayer::videoStarted() {}
void MpvPlayer::newLogMessage(int, const QString&, const QString&) {}

MpvPlayer::MpvPlayer(QObject* impl, const QString& name)
    : d(new Private(this)) {
  d->impl_ = impl;
  d->name_ = name;

  d->mpv_ = mpv_create();
  // Enable default bindings, because we're lazy. Normally, a player using
  // mpv as backend would implement its own key bindings.
  // CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "input-default-bindings",
  // "yes"));
  CHECK_MPV_ERROR(
      mpv_set_option_string(d->mpv_, "input-default-bindings", "no"));
  CHECK_MPV_ERROR(
      mpv_set_option_string(d->mpv_, "input-builtin-bindings", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "input-terminal", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "input-cursor", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "input-media-keys", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "osc", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "osd-bar", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "network-timeout", "0"));

  // Enable keyboard input on the X11 window. For the messy details, see
  // --input-vo-keyboard on the manpage.
  // CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "input-vo-keyboard",
  // "yes"));
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "input-vo-keyboard", "no"));

  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "terminal", "no"));
  CHECK_MPV_ERROR(mpv_set_option_string(
      d->mpv_, "msg-level",
      QLibraryInfo::isDebugBuild() ? "all=debug" : "all=status"));

  // Request hw decoding, just for testing.
  CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "hwdec", "auto-copy"));
#ifdef Q_OS_WINDOWS
  // CHECK_MPV_ERROR(
  //     mpv::qt::set_option_variant(d->mpv_, "hwdec", "d3d11va-copy"));
  // CHECK_MPV_ERROR(
  //     mpv::qt::set_option_variant(d->mpv_, "audio-device", "wasapi"));
  // CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "vo", "gpu"));
  // CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "gpu-context",
  // "d3d11"));
#endif  // Q_OS_WINDOWS

  // Request log messages. They are received as MPV_EVENT_LOG_MESSAGE.
  CHECK_MPV_ERROR(mpv_request_log_messages(
      d->mpv_, QLibraryInfo::isDebugBuild() ? "debug" : "status"));

  d->mpv_event_thread_running_.store(true, std::memory_order_release);
  d->mpv_event_thread_ = std::thread([this] { d->processMpvEvents(); });

  CHECK_MPV_ERROR(mpv_initialize(d->mpv_));

  CHECK_MPV_ERROR(
      mpv_observe_property(d->mpv_, 0, "duration", MPV_FORMAT_DOUBLE));
  CHECK_MPV_ERROR(mpv_observe_property(d->mpv_, 0, "pause", MPV_FORMAT_FLAG));
  CHECK_MPV_ERROR(
      mpv_observe_property(d->mpv_, 0, "eof-reached", MPV_FORMAT_FLAG));
}

MpvPlayer::~MpvPlayer() {
  stop();
  d->mpv_event_thread_running_.store(false, std::memory_order_release);
  mpv_wakeup(d->mpv_);
  if (d->mpv_event_thread_.joinable()) {
    d->mpv_event_thread_.join();
  }
  // mpv_destroy(std::exchange(d->mpv_, nullptr));
  if (d->mpv_) {
    mpv_terminate_destroy(std::exchange(d->mpv_, nullptr));
  }
}

void MpvPlayer::disableAudio() {
  setPlayerProperty("ao", "no");
  setPlayerProperty("aid", "no");
  setPlayerProperty("mute", "yes");
  setPlayerProperty("ao-null-untimed", "yes");
  setPlayerProperty("audio-fallback-to-null", "yes");
}

void MpvPlayer::enableHighPerformanceMode() {
  // Allow frame drop
  setPlayerProperty("framedrop", "vo");
  // setPlayerProperty("framedrop", "yes");

  // Fastest scaling
  setPlayerProperty("scale", "bilinear");

  // Fast sws-scale
  setPlayerProperty("sws-fast", "yes");
  setPlayerProperty("zimg-fast", "yes");
}

QString MpvPlayer::name() const { return d->name_; }

void MpvPlayer::setName(const QString& name) {
  if (name != d->name_) {
    d->name_ = name;
    emit nameChanged(d->name_);
  }
}

void MpvPlayer::setUrl(const QUrl& url) {
  if (url.isEmpty()) {
    return;
  }
  stop();

  if (d->name_.isEmpty()) {
    QString name = url.toString();
    name = name.split("/", Qt::SkipEmptyParts).last();
    name = name.split("\\", Qt::SkipEmptyParts).last();
    setName(name);
  }

  d->url_ = url;
  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "rtsp-transport", "udp"));

  if (d->url_.isLocalFile()) {
    playerCommand("loadfile", d->url_.toLocalFile());
  } else {
    playerCommand("loadfile", d->url_.toString());
  }
  emit urlChanged(d->url_);
}

QUrl MpvPlayer::url() const { return d->url_; }

void MpvPlayer::play(const QUrl& url) {
  if (!url.isEmpty()) {
    setUrl(url);
  }
  resume();
}

struct mpv_handle* MpvPlayer::mpv_handle() const {
  return d->mpv_;
}

void MpvPlayer::pause() { setPlayerProperty("pause", true); }

bool MpvPlayer::isPaused() const { return getPlayerProperty<bool>("pause"); }

void MpvPlayer::setPaused(bool paused) {
  if (isPaused() != paused) {
    paused ? pause() : resume();
    emit pausedChanged(paused);
  }
}

void MpvPlayer::resume() { setPlayerProperty("pause", false); }

void MpvPlayer::stop() { playerCommand("stop"); }

QSize MpvPlayer::videoSize() const {
  return QSize(getPlayerProperty<int>("width"),
               getPlayerProperty<int>("height"));
}

QSize MpvPlayer::displaySize() const {
  return QSize(getPlayerProperty<int>("dwidth"),
               getPlayerProperty<int>("dheight"));
}

void MpvPlayer::setCropVideo(const QRect& rect) {
  // https://ffmpeg.org/ffmpeg-filters.html#crop
  uncropVideo();
  if (rect.isValid()) {
    playerCommand("vf", "add",
                  QStringLiteral("@crop:crop=%1:%2:%3:%4")
                      .arg(rect.width())
                      .arg(rect.height())
                      .arg(rect.x())
                      .arg(rect.y()));
  }
}

void MpvPlayer::setCropVideo(const QRectF& rect_ratio) {
  // https://ffmpeg.org/ffmpeg-filters.html#crop
  uncropVideo();
  if (rect_ratio.isValid()) {
    playerCommand("vf", "add",
                  QStringLiteral("@crop:crop=iw*%1:ih*%2:iw*%3:ih*%4")
                      .arg(rect_ratio.width())
                      .arg(rect_ratio.height())
                      .arg(rect_ratio.x())
                      .arg(rect_ratio.y()));
  }
}

void MpvPlayer::uncropVideo() { playerCommand("vf", "remove", "@crop"); }

QVariant MpvPlayer::command(const QVariant& args) {
  if (d->mpv_) {
    QVariant ret = mpv::qt::command_variant(d->mpv_, args);
    MpvDebug() << "command " << args << ": " << ret;
    if (args.toList().contains("stop")) {
      d->changeState(Stop);
    }
    return ret;
  } else {
    return {};
  }
}

bool MpvPlayer::setPlayerProperty(const QString& name, const QVariant& value) {
  if (d->mpv_) {
    int ret = 0;
    CHECK_MPV_ERROR_RET(ret,
                        mpv::qt::set_property_variant(d->mpv_, name, value));
    MpvDebug() << "setProperty " << name << '=' << value << ": " << ret;
    return ret == 0;
  } else {
    return false;
  }
}

QVariant MpvPlayer::getPlayerProperty_(const QString& name) const {
  if (d->mpv_) {
    QVariant value = mpv::qt::get_property_variant(d->mpv_, name);
    MpvDebug() << "getProperty " << name << ": " << value;
    return value;
  } else {
    return {};
  }
}

void MpvPlayer::processQEvent(QEvent* event) {
  switch (event->type()) {
    case QEvent::LanguageChange:
      break;

    default:
      break;
  }
}

MpvPlayerWidget::MpvPlayerWidget(const QString& name, QWidget* parent,
                                 Qt::WindowFlags f)
    : QWidget(parent, f), MpvPlayer(this, name), d(MpvPlayer::d.get()) {
  CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "vo", "gpu-next"));

  setAttribute(Qt::WA_DontCreateNativeAncestors, true);
  setAttribute(Qt::WA_NativeWindow, true);
  int64_t wid = winId();
  CHECK_MPV_ERROR(mpv_set_option(d->mpv_, "wid", MPV_FORMAT_INT64, &wid));
}

bool MpvPlayerWidget::event(QEvent* event) {
  processQEvent(event);
  return QWidget::event(event);
}

MpvPlayerOpenGLWidget::MpvPlayerOpenGLWidget(const QString& name,
                                             QWidget* parent, Qt::WindowFlags f)
    : QOpenGLWidget(parent, f), MpvPlayer(this, name), d(MpvPlayer::d.get()) {}

MpvPlayerOpenGLWidget::~MpvPlayerOpenGLWidget() {
  makeCurrent();
  mpv_render_context_free(std::exchange(d->mpv_gl_, nullptr));
}

bool MpvPlayerOpenGLWidget::event(QEvent* event) {
  processQEvent(event);
  return QOpenGLWidget::event(event);
}

void MpvPlayerOpenGLWidget::initializeGL() {
  // QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
  // f->glClearColor(118 / 255.0f, 117 / 255.0f, 120 / 255.0f, 1.0f);

  mpv_opengl_init_params gl_init_params{
      &MpvPlayerOpenGLWidget::get_proc_address, this};
  int advanced_control = 1;
  mpv_render_param params[]{
      {MPV_RENDER_PARAM_API_TYPE,
       const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
      {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
      {MPV_RENDER_PARAM_INVALID, nullptr}};

  if (mpv_render_context_create(&d->mpv_gl_, d->mpv_, params) < 0) {
    QMessageBox::critical(this, tr("Cannot initialize MPV"),
                          tr("Failed to initialize mpv GL context"));
    qApp->exit(EXIT_FAILURE);
  }

  mpv_render_context_set_update_callback(d->mpv_gl_,
                                         &MpvPlayerOpenGLWidget::on_update,
                                         reinterpret_cast<void*>(this));
}

void MpvPlayerOpenGLWidget::paintGL() {
  mpv_opengl_fbo mpfbo{int(defaultFramebufferObject()), width(), height(), 0};
  int flip_y = 1;

  mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
                               {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
                               {MPV_RENDER_PARAM_INVALID, nullptr}};
  // See render_gl.h on what OpenGL environment mpv expects, and
  // other API details.
  mpv_render_context_render(d->mpv_gl_, params);
}

void* MpvPlayerOpenGLWidget::get_proc_address(void* ctx, const char* name) {
  MpvPlayerOpenGLWidget* canvas = static_cast<MpvPlayerOpenGLWidget*>(ctx);
  QOpenGLContext* glctx = canvas->context();
  if (glctx) {
    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
  } else {
    return nullptr;
  }
}

void MpvPlayerOpenGLWidget::on_update(void* cb_ctx) {
  MpvPlayerOpenGLWidget* canvas = static_cast<MpvPlayerOpenGLWidget*>(cb_ctx);
  QMetaObject::invokeMethod(canvas, [canvas] { canvas->maybeUpdate(); });
}

// Make Qt invoke mpv_render_context_render() to draw a
// new/updated video frame.
void MpvPlayerOpenGLWidget::maybeUpdate() {
  // If the Qt window is not visible, Qt's update() will just skip
  // rendering. This confuses mpv's render API, and may lead to
  // small occasional freezes due to video rendering timing out.
  // Handle this by manually redrawing.
  // Note: Qt doesn't seem to provide a way to query whether
  // update() will
  //       be skipped, and the following code still fails when
  //       e.g. switching to a different workspace with a
  //       reparenting window manager.
  if (window()->isMinimized()) {
    makeCurrent();
    paintGL();
    context()->swapBuffers(context()->surface());
    doneCurrent();
  } else {
    update();
  }
}

class MpvPlayerQuickObject::MpvQuickRenderer
    : public QQuickFramebufferObject::Renderer {
 public:
  MpvQuickRenderer(MpvPlayerQuickObject* parent) : obj(parent), d(obj->d) {
    mpv_set_wakeup_callback(
        d->mpv_, [](void* ctx) { Q_UNUSED(ctx) }, nullptr);
  }

  ~MpvQuickRenderer() override {}

  // This function is called when a new FBO is needed.
  // This happens on the initial frame.
  QOpenGLFramebufferObject* createFramebufferObject(
      const QSize& size) override {
    // init mpv_gl:
    if (!d->mpv_gl_) {
      mpv_opengl_init_params gl_init_params{
          MpvPlayerQuickObject::get_proc_address, nullptr};
      int advanced_control = 1;
      mpv_render_param params[]{
          {MPV_RENDER_PARAM_API_TYPE,
           const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
          {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
          {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
          {MPV_RENDER_PARAM_INVALID, nullptr}};

      if (mpv_render_context_create(&d->mpv_gl_, d->mpv_, params) < 0) {
        throw std::runtime_error("failed to initialize mpv GL context");
      }
      mpv_render_context_set_update_callback(
          d->mpv_gl_,
          [](void* ctx) {
            QMetaObject::invokeMethod(static_cast<MpvPlayerQuickObject*>(ctx),
                                      &MpvPlayerQuickObject::update,
                                      Qt::QueuedConnection);
          },
          obj);
    }

    return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
  }

  void render() override {
    obj->window()->resetOpenGLState();

    QOpenGLFramebufferObject* fbo = framebufferObject();
    mpv_opengl_fbo mpfbo{int(fbo->handle()), fbo->width(), fbo->height(), 0};
    int flip_y = 0;

    mpv_render_param params[] = {
        // Specify the default framebuffer (0) as target. This will
        // render onto the entire screen. If you want to show the video
        // in a smaller rectangle or apply fancy transformations, you'll
        // need to render into a separate FBO and draw it manually.
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
        // Flip rendering (needed due to flipped GL coordinate system).
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    // See render_gl.h on what OpenGL environment mpv expects, and
    // other API details.
    mpv_render_context_render(d->mpv_gl_, params);

    obj->window()->resetOpenGLState();
  }

 private:
  MpvPlayerQuickObject* obj;
  MpvPlayer::Private* d;
};

MpvPlayerQuickObject::MpvPlayerQuickObject(const QString& name,
                                           QQuickItem* parent)
    : QQuickFramebufferObject(parent),
      MpvPlayer(this, name),
      d(MpvPlayer::d.get()) {}

MpvPlayerQuickObject::~MpvPlayerQuickObject() {
  if (d->mpv_gl_) {
    mpv_render_context_free(std::exchange(d->mpv_gl_, nullptr));
  }
}

MpvPlayerQuickObject::Renderer* MpvPlayerQuickObject::createRenderer() const {
  window()->setPersistentOpenGLContext(true);
  window()->setPersistentSceneGraph(true);
  return new MpvQuickRenderer(const_cast<MpvPlayerQuickObject*>(this));
}

void* MpvPlayerQuickObject::get_proc_address(void* ctx, const char* name) {
  Q_UNUSED(ctx)
  QOpenGLContext* glctx = QOpenGLContext::currentContext();
  if (glctx) {
    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
  } else {
    return nullptr;
  }
}
