#ifndef DOWNLOADITEMDELEGATE_H
#define DOWNLOADITEMDELEGATE_H

#include <QStyledItemDelegate>

class DownloadItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit DownloadItemDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    void paintProgressBar(QPainter* painter, const QStyleOptionViewItem& option, int progress) const;
};

#endif
