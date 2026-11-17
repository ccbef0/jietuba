#pragma once
#include "ocr/ocr_manager.h"
#include "translation/models.h"
#include "translation/service.h"
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QVariantMap>
#include <QVector>
#include <QWidget>
#include <memory>

namespace jietuba {
class InputHub;
class TranslationWorker;

// One recognized line, moved into the screen coordinates it was captured from, with the
// translation drawn over it.
struct InplaceLine {
    QRectF box;
    QString text;
};

// The translated text laid where the original was, instead of in a window beside it.
//
// One click-through card per recognized line: the line's own box gets an opaque background,
// so the source text underneath is covered rather than left beside the translation. The card
// is transparent to input and never takes focus, which is what lets the next click anywhere
// dismiss it instead of the user hunting for a close button.
class InPlaceTranslationOverlay : public QWidget {
public:
    explicit InPlaceTranslationOverlay();
    // The waiting note shown while recognition and translation run. Empty clears it.
    void set_status(const QString &message);
    // Fills the cards; an empty list leaves the overlay with nothing but its status.
    void set_lines(const QVector<InplaceLine> &lines);
    // Shows the overlay over the captured region, widened by a margin so a translation
    // longer than its source line still has room.
    void show_over(const QRect &region);
    void dismiss();
    bool showing() const { return isVisible(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static constexpr int MARGIN = 28;
    static constexpr int CARD_PADDING = 4;
    static constexpr int CARD_RADIUS = 5;
    // How far past the recognized box the card reaches, so no stroke of the original line is
    // left outside it.
    static constexpr int BOX_SLACK = 3;
    static constexpr double MIN_PIXEL_SIZE = 7.0;

    QVector<InplaceLine> lines_;
    QString status_;
    QRect region_;
    // Where the overlay really landed, which is not always the region widened by the margin:
    // near a screen edge the widened area is clipped and starts earlier than the margin.
    QPoint origin_;
};

// Runs the recognition and the translation of a dragged region off the GUI thread and hands
// the result to the overlay. Owned by one instance for the whole run: a new region replaces
// what the previous one was still doing.
class InplaceTranslation : public QObject {
    Q_OBJECT
public:
    static InplaceTranslation *instance();
    // Waits for the threads of the request in flight, then closes the overlay.
    static void cleanup();
    // Recognizes the captured image and translates it in place over region.
    void start(const QImage &image, const QRect &region, const QVariantMap &params);
    // Hides the overlay and stops watching for the click that dismisses it.
    void dismiss();

private:
    explicit InplaceTranslation(QObject *parent = nullptr);
    ~InplaceTranslation() override;

    void on_ocr_finished();
    void on_translation_finished(const TranslationResult &result);
    void show_status(const QString &message);
    // Hides the overlay after ms unless a newer request has taken it over by then.
    void hide_soon(quint64 token, int ms);
    // Watches global mouse and keyboard input so the overlay goes away on its own.
    void watch_input(bool on);
    void stop_threads();
    std::shared_ptr<TranslationService> service();

    class OcrThread;

    InPlaceTranslationOverlay overlay_;
    std::shared_ptr<TranslationService> service_;
    OcrThread *ocr_thread_ = nullptr;
    TranslationWorker *worker_ = nullptr;
    // The recognized lines whose translation is being waited for, in reading order.
    QVector<OcrTextLine> source_lines_;
    QRect region_;
    QSize image_size_;
    QString target_lang_;
    // Invalidates the results of a request a newer one has already replaced.
    quint64 token_ = 0;
    QMetaObject::Connection mouse_connection_;
    QMetaObject::Connection key_connection_;
    bool watching_ = false;
};
}
