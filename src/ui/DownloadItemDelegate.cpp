#include "ui/DownloadItemDelegate.h"
#include <QPainter>
#include <QStyleOptionProgressBar>
#include <QApplication>
#include <QLinearGradient>
#include <QPen>
#include <QBrush>

DownloadItemDelegate::DownloadItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void DownloadItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    painter->save();

    if (index.column() == 3) {
        int progress = index.data(Qt::DisplayRole).toInt();
        paintProgressBar(painter, option, progress);
    } else {
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, option.palette.highlight());
            painter->setPen(option.palette.highlightedText().color());
        } else {
            painter->setPen(option.palette.text().color());
        }

        QStyleOptionViewItem opt = option;
        opt.rect = opt.rect.adjusted(4, 0, -4, 0);
        QStyledItemDelegate::paint(painter, opt, index);
    }

    painter->restore();
}

void DownloadItemDelegate::paintProgressBar(QPainter* painter, const QStyleOptionViewItem& option, int progress) const {
    QRect rect = option.rect.adjusted(4, 8, -4, -8);

    painter->setRenderHint(QPainter::Antialiasing);

    QColor bgColor(60, 60, 60);
    painter->setPen(Qt::NoPen);
    painter->setBrush(bgColor);
    painter->drawRoundedRect(rect, 4, 4);

    if (progress > 0) {
        QRect progressRect(rect);
        progressRect.setWidth(rect.width() * progress / 100);

        QColor progressColor;
        if (progress >= 100) {
            progressColor = QColor(0, 180, 0);
        } else {
            progressColor = QColor(0, 120, 220);
        }

        QLinearGradient gradient(progressRect.topLeft(), progressRect.bottomLeft());
        gradient.setColorAt(0, progressColor.lighter(120));
        gradient.setColorAt(1, progressColor);

        painter->setBrush(gradient);
        painter->drawRoundedRect(progressRect, 4, 4);
    }

    painter->setPen(Qt::white);
    painter->setBrush(Qt::NoBrush);
    QFont font = painter->font();
    font.setPointSize(9);
    painter->setFont(font);
    painter->drawText(rect, Qt::AlignCenter, QString::number(progress) + "%");
}

QSize DownloadItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(size.height(), 40));
    return size;
}
