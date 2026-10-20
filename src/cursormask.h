// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pure Qt helper (no KWin dependency): splits a cursor image into the pixels that
// get inverted and everything else. Header-only so the build files don't change.
#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QSize>

#include <algorithm>

namespace XorCursorDetail
{

class CursorMask
{
public:
    CursorMask() = default;

    /**
     * @param threshold pixels with alpha >= threshold (1..255) are inverted
     * @param keepRest  also keep the remaining, partly transparent pixels
     *                  so they can be drawn the way the cursor theme made them
     */
    static CursorMask build(const QImage &source, int threshold, bool keepRest)
    {
        CursorMask mask;
        if (source.isNull()) {
            return mask;
        }
        mask.m_size = source.size();
        threshold = std::clamp(threshold, 1, 255);

        // Premultiplication doesn't change alpha, so thresholding is unaffected.
        QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        // Runs of inverted pixels with the same x and width in consecutive rows are
        // merged into one rect, so a typical cursor needs only a few dozen rects.
        QHash<quint64, int> open;
        QHash<quint64, int> next;
        bool hasRest = false;

        const int width = image.width();
        for (int y = 0; y < image.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            next.clear();
            int x = 0;
            while (x < width) {
                const int alpha = qAlpha(line[x]);
                if (alpha >= threshold) {
                    const int start = x;
                    while (x < width && qAlpha(line[x]) >= threshold) {
                        line[x] = 0;
                        ++x;
                    }
                    const quint64 key = (quint64(start) << 32) | quint32(x - start);
                    const auto it = open.constFind(key);
                    if (it != open.constEnd()) {
                        mask.m_rects[*it].setHeight(mask.m_rects[*it].height() + 1);
                        next.insert(key, *it);
                    } else {
                        mask.m_rects.append(QRect(start, y, x - start, 1));
                        next.insert(key, mask.m_rects.size() - 1);
                    }
                } else {
                    hasRest = hasRest || alpha > 0;
                    ++x;
                }
            }
            std::swap(open, next);
        }

        if (keepRest && hasRest) {
            image.setDevicePixelRatio(source.devicePixelRatio());
            mask.m_rest = image;
        }
        return mask;
    }

    bool hasInvertedPixels() const
    {
        return !m_rects.isEmpty();
    }

    // Non-overlapping rects of the inverted pixels, in image pixels.
    const QList<QRect> &rects() const
    {
        return m_rects;
    }

    // Premultiplied ARGB32 image with the inverted pixels cleared, or a null image
    // when nothing partly transparent is left (or keepRest was false).
    const QImage &rest() const
    {
        return m_rest;
    }

    // The inverted rects scaled to a device-pixel size, rounded outward and merged so
    // that they never overlap (subtract blending would otherwise apply twice).
    QList<QRect> scaled(const QSize &target) const
    {
        if (m_size.isEmpty() || target.isEmpty()) {
            return {};
        }
        if (target == m_size) {
            return m_rects;
        }
        const qreal fx = qreal(target.width()) / m_size.width();
        const qreal fy = qreal(target.height()) / m_size.height();
        QRegion region;
        for (const QRect &r : m_rects) {
            region += QRectF(r.x() * fx, r.y() * fy, r.width() * fx, r.height() * fy).toAlignedRect();
        }
        QList<QRect> out;
        for (const QRect &r : region) {
            out.append(r);
        }
        return out;
    }

private:
    QSize m_size;
    QList<QRect> m_rects;
    QImage m_rest;
};

} // namespace XorCursorDetail
