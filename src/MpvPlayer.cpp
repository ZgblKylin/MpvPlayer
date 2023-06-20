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
#define MpvLog(log) log(MPV).nospace().noquote() << '[' << d->name_ << "] "
#define MpvDebug() MpvLog(qCDebug)
#define MpvInfo() MpvLog(qCInfo)
#define MpvWarning() MpvLog(qCWarning)
#define MpvCritical() MpvLog(qCCritical)
#define MpvFatal(format, ...)                                                \
  QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, \
                 MPV().categoryName())                                       \
      .fatal("[%s] " format, qPrintable(d->name_), ##__VA_ARGS__)

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
  mpv_handle* mpv_ = nullptr;
  mpv_render_context* mpv_gl_ = nullptr;
};

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
      QLibraryInfo::isDebugBuild() ? "all=v" : "all=status"));

  // Request hw decoding, just for testing.
  CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "hwdec", "auto"));
#ifdef Q_OS_WINDOWS
  CHECK_MPV_ERROR(
      mpv::qt::set_option_variant(d->mpv_, "hwdec", "d3d11va-copy"));
  CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "ao", "null"));
  // CHECK_MPV_ERROR(
  //     mpv::qt::set_option_variant(d->mpv_, "audio-device", "wasapi"));
  // CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "vo", "gpu"));
  // CHECK_MPV_ERROR(mpv::qt::set_option_variant(d->mpv_, "gpu-context",
  // "d3d11"));
#endif  // Q_OS_WINDOWS

  CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "framedrop", "decoder+vo"));
  // CHECK_MPV_ERROR(mpv_set_option_string(d->mpv_, "framedrop", "yes"));

  // Request log messages. They are received as MPV_EVENT_LOG_MESSAGE.
  CHECK_MPV_ERROR(mpv_request_log_messages(
      d->mpv_, QLibraryInfo::isDebugBuild() ? "v" : "status"));

  // From this point on, the wakeup function will be called. The callback
  // can come from any thread, so we use the QueuedConnection mechanism to
  // relay the wakeup in a thread-safe way.
  mpv_set_wakeup_callback(
      d->mpv_,
      [](void* ctx) {
        MpvPlayer* self = static_cast<MpvPlayer*>(ctx);
        QMetaObject::invokeMethod(
            self->d->impl_, [self] { self->processMpvEvents(); },
            Qt::QueuedConnection);
      },
      this);

  CHECK_MPV_ERROR(mpv_initialize(d->mpv_));
}

MpvPlayer::~MpvPlayer() {
  stop();
  // mpv_destroy(std::exchange(d->mpv_, nullptr));
  if (d->mpv_) {
    mpv_terminate_destroy(std::exchange(d->mpv_, nullptr));
  }
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
  setPlayerProperty("pause", false);
}

void MpvPlayer::pause() { setPlayerProperty("true", false); }

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

void MpvPlayer::uncropVideo() { playerCommand("vf", "remove", "@crop"); }

QVariant MpvPlayer::command(const QVariant& args) {
  if (d->mpv_) {
    QVariant ret = mpv::qt::command_variant(d->mpv_, args);
    MpvDebug() << "command " << args << ": " << ret;
    if (args.toList().contains("stop")) {
      emit playStateChanged(Stop);
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

void MpvPlayer::processMpvEvents() {
  // Process all events, until the event queue is empty.
  while (d->mpv_) {
    mpv_event* event = mpv_wait_event(d->mpv_, 0);
    if (event->event_id == MPV_EVENT_NONE) {
      break;
    }

    switch (event->event_id) {
      case MPV_EVENT_START_FILE: {  /// 6: Notification before playback start of
                                    /// a file (before the file is loaded).* See
                                    /// also mpv_event and mpv_event_start_file.
      } break;

      case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property* prop = (mpv_event_property*)event->data;
        // qDebug()<<"prop->name:"<<QString(prop->name);
        if (strcmp(prop->name, "duration") == 0) {
          if (prop->format == MPV_FORMAT_DOUBLE) {
            double time = *(double*)prop->data;
            emit durationChanged(time);
          }
        } else if (strcmp(prop->name, "pause") == 0) {
          if (prop->format == MPV_FORMAT_FLAG) {
            int flag = *(int*)prop->data;
            emit playStateChanged(flag ? Play : Stop);
          }
        } else if (strcmp(prop->name, "eof-reached") == 0) {
          if (prop->format == MPV_FORMAT_FLAG) {
            int flag = *(int*)prop->data;
            if (flag) {
              emit playStateChanged(EndReached);
            }
          }
        }
        break;
      }
      case MPV_EVENT_FILE_LOADED: {
        QTimer::singleShot(5, d->impl_, [this]() {
          int64_t width, height;
          // mpv_get_property(d->mpv_, "dwidth", MPV_FORMAT_INT64, &width);
          // mpv_get_property(d->mpv_, "dheight", MPV_FORMAT_INT64, &height);
          emit videoStarted();
        });
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

        static const QMap<int, QtMsgType> kMpvToQtMsgType = {
            {MPV_LOG_LEVEL_FATAL, QtFatalMsg},
            {MPV_LOG_LEVEL_ERROR, QtCriticalMsg},
            {MPV_LOG_LEVEL_WARN, QtWarningMsg},
            {MPV_LOG_LEVEL_INFO, QtInfoMsg},
            {MPV_LOG_LEVEL_V, QtDebugMsg},
            {MPV_LOG_LEVEL_DEBUG, QtDebugMsg},
            {MPV_LOG_LEVEL_TRACE, QtDebugMsg},
        };
        auto it = kMpvToQtMsgType.find(msg->log_level);
        if (it == kMpvToQtMsgType.cend()) {
          continue;
        }
        QtMsgType level = it.value();

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
        switch (level) {
          case QtFatalMsg:
            MpvFatal("%s", qPrintable(message));
            break;
          case QtCriticalMsg:
            MpvCritical() << message;
            break;
          case QtWarningMsg:
            MpvWarning() << message;
            break;
          case QtInfoMsg:
            MpvInfo() << message;
            break;
          case QtDebugMsg:
            MpvDebug() << message;
            break;
          default:
            continue;
        }

        emit newLogMessage(level, prefix, text);
      } break;

      case MPV_EVENT_SHUTDOWN: {
        if (d->mpv_) {
          mpv_terminate_destroy(std::exchange(d->mpv_, nullptr));
        }
      } break;

      default:
        // Ignore uninteresting or unknown events.
        break;
    }
  }
}

MpvPlayerWidget::MpvPlayerWidget(const QString& name, QWidget* parent,
                                 Qt::WindowFlags f)
    : QWidget(parent, f), MpvPlayer(this, name), d(MpvPlayer::d.get()) {
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
