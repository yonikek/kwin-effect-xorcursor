#pragma once
#include <QImage>
#include <QPointF>
namespace KWin {
struct CursorImage {
    CursorImage() { m_image = QImage(32, 32, QImage::Format_ARGB32_Premultiplied); m_image.setDevicePixelRatio(1.0); }
    QImage image() const { return m_image; }
    QPointF hotSpot() const { return m_hotSpot; }
    QImage m_image;
    QPointF m_hotSpot;
};
}
