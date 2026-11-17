#include "translation/inplace_translation.h"
#include "core/i18n.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "ocr/ocr_manager.h"
#include "settings/tool_settings.h"
#include "translation/worker.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <exception>
#include <optional>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr const char *WATCHER = "inplace_translation";
constexpr double MAX_PIXEL_SIZE = 44.0;

// The one instance, made on the first in-place translation and gone at exit.
InplaceTranslation *self_ = nullptr;

// The desktop in the coordinates widgets use, as the drag selection computes it.
QRect desktop_geometry() {
    QRect bounds;
    for (auto *screen : QApplication::screens()) bounds = bounds.united(screen->geometry());
    return bounds;
}

// Light cards on a dark theme and dark cards on a light one, so the translation stays legible
// over whatever it covers.
void card_colors(const QString &theme_mode, QColor &card, QColor &ink) {
    const bool light = theme_mode == "light";
    card = light ? QColor(250, 250, 250, 238) : QColor(26, 26, 28, 232);
    ink = light ? QColor(24, 24, 26) : QColor(246, 246, 246);
}

// The translated text cut back into one entry per source line.
//
// Providers are asked to keep the line breaks and the LLM ones do, but a traditional engine
// still merges a wrapped sentence or splits one line into two. When the counts disagree the
// translation is shared out by how much of the source each line carries, so every line keeps
// a piece of the text at its own place instead of the whole block landing on the first one.
QStringList align_translation(const QString &translated, const QVector<QString> &sources) {
    QStringList parts;
    for (const auto &line : translated.split('\n')) {
        const auto trimmed = line.trimmed();
        if (!trimmed.isEmpty()) parts << trimmed;
    }
    const int count = int(sources.size());
    if (count == 0) return {};
    if (parts.size() == count) return parts;
    if (parts.isEmpty()) return QStringList(count, QString());
    const QString joined = parts.join(' ');
    const auto units = joined.toUcs4();
    QVector<int> weights;
    weights.reserve(count);
    int total_weight = 0;
    for (const auto &source : sources) {
        const int weight = qMax(1, int(source.toUcs4().size()));
        weights << weight;
        total_weight += weight;
    }
    QStringList aligned;
    aligned.reserve(count);
    qsizetype consumed = 0;
    for (int i = 0; i < count; ++i) {
        qsizetype take = i == count - 1 ? units.size() - consumed : qsizetype(std::llround(double(units.size()) * weights[i] / total_weight));
        // A piece that starts inside a word would cut it in half; step back to the space before
        // it when one is close. Writing systems without spaces have nothing to fix.
        if (i > 0 && consumed > 0 && consumed < units.size() && units[consumed] != U' ') {
            qsizetype back = consumed;
            const qsizetype limit = qMax(qsizetype(0), consumed - 12);
            while (back > limit && units[back] != U' ') --back;
            if (back > limit) consumed = back + 1;
        }
        take = qBound(qsizetype(0), take, units.size() - consumed);
        aligned << QString::fromUcs4(units.constData() + consumed, take);
        consumed += take;
    }
    return aligned;
}
}

InPlaceTranslationOverlay::InPlaceTranslationOverlay() : QWidget(nullptr) {
    // The same decoration-free, input-transparent, never-activated window the drag selection
    // uses: it sits over the screen without ever becoming the window under the cursor.
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);
}

void InPlaceTranslationOverlay::set_status(const QString &message) {
    status_ = message;
    update();
}

void InPlaceTranslationOverlay::set_lines(const QVector<InplaceLine> &lines) {
    lines_ = lines;
    status_.clear();
    update();
}

void InPlaceTranslationOverlay::show_over(const QRect &region) {
    region_ = region;
    const QRect bounds = desktop_geometry();
    QRect area = region.adjusted(-MARGIN, -MARGIN, MARGIN, MARGIN);
    if (!bounds.isNull()) area = area.intersected(bounds);
    if (area.isEmpty()) area = region;
    setGeometry(area);
    show();
    update();
}

void InPlaceTranslationOverlay::dismiss() {
    hide();
    lines_.clear();
    status_.clear();
}

void InPlaceTranslationOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QColor card, ink;
    card_colors(get_tool_settings_manager()->get_ui_theme_mode(), card, ink);
    const QFont base = font();
    const double right_edge = width();

    if (!status_.isEmpty()) {
        QFont font = base;
        font.setPixelSize(14);
        QFontMetricsF metrics(font);
        const double pill_width = qMin(metrics.horizontalAdvance(status_) + 18.0, right_edge - 8);
        const QRectF pill(4, 4, qMax(pill_width, 48.0), metrics.height() + 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(card);
        painter.drawRoundedRect(pill, CARD_RADIUS, CARD_RADIUS);
        painter.setPen(ink);
        painter.setFont(font);
        painter.drawText(pill, Qt::AlignCenter | Qt::TextSingleLine | Qt::TextDontShowMnemonic, status_);
    }

    for (const auto &line : lines_) {
        if (line.text.isEmpty() || line.box.isNull()) continue;
        const QRectF box = line.box.translated(-region_.topLeft());
        if (box.right() < 0 || box.bottom() < 0 || box.left() > right_edge) continue;

        QFont font = base;
        double size = qBound(MIN_PIXEL_SIZE, box.height() * 0.86, MAX_PIXEL_SIZE);
        font.setPixelSize(int(size));
        QFontMetricsF metrics(font);
        const double inner = box.width() - 2 * CARD_PADDING;
        while (size > MIN_PIXEL_SIZE && metrics.horizontalAdvance(line.text) > inner) {
            size = qMax(MIN_PIXEL_SIZE, size - 1.0);
            font.setPixelSize(int(size));
            metrics = QFontMetricsF(font);
        }
        // Shrinking alone cannot fit a translation much longer than its source: the card grows
        // towards the right edge instead, and only text past that is cut with an ellipsis.
        const double available = qMax(24.0, right_edge - box.left() - 4);
        const double needed = metrics.horizontalAdvance(line.text) + 2 * CARD_PADDING + 2;
        const double card_width = needed > box.width() ? qMin(needed, available) : box.width();
        const QRectF target(box.left(), box.top(), card_width, qMax(box.height(), metrics.height() + 4.0));

        painter.setPen(Qt::NoPen);
        painter.setBrush(card);
        painter.drawRoundedRect(target.adjusted(-1, -1, 1, 1), CARD_RADIUS, CARD_RADIUS);
        painter.setPen(ink);
        painter.setFont(font);
        const QRectF text_rect = target.adjusted(CARD_PADDING, 0, -CARD_PADDING, 0);
        const QString shown =
            metrics.horizontalAdvance(line.text) > text_rect.width() ? metrics.elidedText(line.text, Qt::ElideRight, int(text_rect.width())) : line.text;
        painter.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine | Qt::TextDontShowMnemonic, shown);
    }
}

// The recognized lines, in the image's own pixels. No signal of its own: the GUI thread reads
// them once the thread has finished, so nothing but a plain type crosses a queued connection.
class InplaceTranslation::OcrThread : public QThread {
public:
    OcrThread(const QImage &image, QObject *parent) : QThread(parent), image_(image) {}
    // Stops PP-OCR at its next tile or line; the result is dropped either way.
    void cancel() {
        cancelled_ = true;
        requestInterruption();
    }
    QVector<OcrTextLine> lines;
    QString error;

protected:
    void run() override {
        try {
            const auto result = recognize_text(image_, &cancelled_);
            if (result.code != 100) error = result.msg.isEmpty() ? result.reason : result.msg;
            else lines = group_ocr_lines(result);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        } catch (...) {
            error = QStringLiteral("Recognition failed");
        }
    }

private:
    QImage image_;
    std::atomic_bool cancelled_ = false;
};

InplaceTranslation *InplaceTranslation::instance() {
    if (!self_) self_ = new InplaceTranslation();
    return self_;
}

void InplaceTranslation::cleanup() {
    auto *previous = self_;
    if (!previous) return;
    self_ = nullptr;
    previous->dismiss();
    if (previous->ocr_thread_) previous->ocr_thread_->wait(3000);
    if (previous->worker_) previous->worker_->wait(3000);
    delete previous;
}

InplaceTranslation::InplaceTranslation(QObject *parent) : QObject(parent) {}

InplaceTranslation::~InplaceTranslation() {
    watch_input(false);
    stop_threads();
}

std::shared_ptr<TranslationService> InplaceTranslation::service() {
    if (!service_) service_ = create_default_translation_service();
    return service_;
}

void InplaceTranslation::start(const QImage &image, const QRect &region, const QVariantMap &params) {
    if (image.isNull() || region.isEmpty()) return;
    stop_threads();
    const quint64 token = ++token_;
    region_ = region;
    image_size_ = image.size();
    target_lang_ = params.value("target_lang", "ZH").toString();
    source_lines_.clear();
    overlay_.set_lines({});
    watch_input(true);
    show_status(i18n_tr("Recognizing...", "TranslationLoadingDialog"));

    auto *thread = new OcrThread(image.copy(), this);
    ocr_thread_ = thread;
    // The result first and the self-deletion last: both are queued on the same signal, so this
    // order is what keeps the thread alive while the result is read.
    connect(thread, &QThread::finished, this,
            [this, thread, token] {
                if (token != token_) return;
                on_ocr_finished();
                if (ocr_thread_ == thread) ocr_thread_ = nullptr;
            },
            Qt::QueuedConnection);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void InplaceTranslation::on_ocr_finished() {
    if (!ocr_thread_) return;
    if (!ocr_thread_->error.isEmpty()) {
        show_status(ocr_thread_->error);
        hide_soon(token_, 2500);
        return;
    }
    QVector<OcrTextLine> kept;
    for (const auto &line : ocr_thread_->lines) {
        if (line.text.trimmed().isEmpty()) continue;
        kept.append(line);
    }
    if (kept.isEmpty()) {
        show_status(i18n_tr("No text recognized", "TranslationDialog"));
        hide_soon(token_, 2500);
        return;
    }
    source_lines_ = kept;
    QStringList texts;
    for (const auto &line : kept) texts << line.text;
    // One request for the whole block: the lines travel as line breaks, which both the LLM
    // engines and DeepL's formatting flag are asked to keep. Merging sentences would lose which
    // translation belongs to which line, so the user's "ignore line breaks" option is ignored
    // here.
    const TranslationRequest request(texts.join('\n'), target_lang_, std::nullopt, true, 10, {{"split_sentences", "0"}});
    auto *worker = new TranslationWorker(*service(), request, {}, {}, this);
    worker_ = worker;
    const quint64 token = token_;
    connect(worker, &TranslationWorker::finished_signal, this, [this, token](const TranslationResult &result) {
        if (token == token_) on_translation_finished(result);
    });
    connect(worker, &QThread::finished, this, [this, worker] {
        if (worker_ == worker) worker_ = nullptr;
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void InplaceTranslation::on_translation_finished(const TranslationResult &result) {
    const quint64 token = token_;
    QVector<QString> sources;
    for (const auto &line : source_lines_) sources << line.text;
    const auto aligned = align_translation(result.translated_text, sources);
    QString joined;
    for (const auto &piece : aligned) joined += piece;
    if (joined.trimmed().isEmpty()) {
        show_status(result.error_message.isEmpty() ? i18n_tr("Translation failed", "TranslationDialog") : result.error_message);
        hide_soon(token_, 2500);
        return;
    }
    // The image and the region disagree in size under display scaling; the ratio between them
    // is what turns a pixel of the recognized picture into a pixel of the screen.
    const double sx = image_size_.width() > 0 ? double(region_.width()) / image_size_.width() : 1.0;
    const double sy = image_size_.height() > 0 ? double(region_.height()) / image_size_.height() : 1.0;
    QVector<InplaceLine> lines;
    for (int i = 0; i < source_lines_.size() && i < aligned.size(); ++i) {
        const auto &source = source_lines_[i];
        QRectF box(source.left * sx, source.top * sy, (source.right - source.left) * sx, (source.bottom - source.top) * sy);
        box.translate(region_.topLeft());
        if (box.width() < 4 || box.height() < 4 || aligned[i].isEmpty()) continue;
        lines.append(InplaceLine{box, aligned[i]});
    }
    if (lines.isEmpty()) {
        dismiss();
        return;
    }
    overlay_.set_lines(lines);
    overlay_.show_over(region_);
    // A card nobody clicks would otherwise sit on the screen until the next drag. Five minutes
    // is long enough to read anything and short enough that a forgotten one clears itself.
    hide_soon(token, 300000);
    log_info(QString("In-place translation drew %1 line(s) over %2x%3").arg(lines.size()).arg(region_.width()).arg(region_.height()), "Translation");
}

void InplaceTranslation::show_status(const QString &message) {
    overlay_.set_status(message);
    overlay_.show_over(region_);
}

void InplaceTranslation::hide_soon(quint64 token, int ms) {
    QTimer::singleShot(ms, this, [this, token] {
        if (token == token_) dismiss();
    });
}

void InplaceTranslation::dismiss() {
    watch_input(false);
    overlay_.dismiss();
}

void InplaceTranslation::stop_threads() {
    // The token already drops whatever these two report; aborting only frees the network and
    // the model sooner. Neither thread is waited for: the GUI thread never blocks on them.
    if (ocr_thread_) ocr_thread_->cancel();
    if (worker_ && worker_->isRunning()) worker_->abort();
}

void InplaceTranslation::watch_input(bool on) {
    if (on == watching_) return;
    auto *hub = on ? input_hub() : existing_input_hub();
    watching_ = on;
    if (!hub) return;
    if (!on) {
        disconnect(mouse_connection_);
        disconnect(key_connection_);
        hub->native->unwatch_mouse(WATCHER);
        hub->native->unwatch_keys(WATCHER);
        return;
    }
    // The press that ends this is never the drag's: the button was released before the result
    // arrived, so the first press afterwards - wherever it lands - is the one that means
    // "enough". Nothing is swallowed; the click still reaches the application under it.
    mouse_connection_ = connect(hub, &InputHub::mouse_button, this,
                                [this](const QString &watcher, const QString &, bool pressed, int, int) {
                                    if (watcher == WATCHER && pressed) dismiss();
                                },
                                Qt::QueuedConnection);
    hub->native->watch_mouse(WATCHER);
    key_connection_ = connect(hub, &InputHub::key, this,
                              [this](const QString &watcher, int key, bool pressed) {
                                  if (watcher == WATCHER && pressed && key == VK_ESCAPE) dismiss();
                              },
                              Qt::QueuedConnection);
    hub->native->watch_keys(WATCHER, {quint32(VK_ESCAPE)});
}
}
