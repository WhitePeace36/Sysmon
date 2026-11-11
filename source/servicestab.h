// Services tab: every systemd service unit (system or user scope) with its
// state, the usual start/stop/restart/enable/disable/mask/unmask actions and
// the `systemctl status` output of the selected unit.
//
// Everything goes through the systemctl command line tool. For system
// services systemctl asks polkit, so the desktop's authentication dialog
// appears when an action needs administrator rights.
#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPoint;
class QSettings;
class QTimer;
class RowSortProxy;
class TypeAheadView;

struct ServiceRow {
    QString unit;
    QString load;       // loaded, not-found, masked, ...
    QString active;     // active, inactive, failed, activating, ...
    QString sub;        // running, exited, dead, ...
    QString fileState;  // enabled, disabled, static, masked, ...
    QString description;
};

class ServiceModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Unit, Active, Sub, FileState, Load, Description, Count };

    explicit ServiceModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setRows(const QVector<ServiceRow>& rows);
    const ServiceRow* rowAt(int row) const;

private:
    QVector<ServiceRow> m_rows;
};

class ServicesTab : public QWidget {
    Q_OBJECT

public:
    explicit ServicesTab(QWidget* parent = nullptr);

    void saveState(QSettings& settings) const;
    void restoreState(QSettings& settings);

public slots:
    void reload();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onCurrentChanged();
    void showContextMenu(const QPoint& pos);

private:
    using Done = std::function<void(int exitCode, const QString& out, const QString& err)>;

    void run(const QStringList& arguments, const Done& done);
    QStringList scopeArguments() const;
    void apply(const QString& units, const QString& unitFiles);
    void perform(const QString& verb);
    void refreshStatus();
    void updateActions();
    const ServiceRow* current() const;
    QAction* makeAction(const QString& text, const QString& verb, const QString& tip);

    QComboBox* m_scope = nullptr;
    QLineEdit* m_filter = nullptr;
    QLabel* m_summary = nullptr;
    TypeAheadView* m_view = nullptr;
    ServiceModel* m_model = nullptr;
    RowSortProxy* m_proxy = nullptr;
    QPlainTextEdit* m_status = nullptr;
    QTimer* m_timer = nullptr;

    QAction* m_start = nullptr;
    QAction* m_stop = nullptr;
    QAction* m_restart = nullptr;
    QAction* m_enable = nullptr;
    QAction* m_disable = nullptr;
    QAction* m_mask = nullptr;
    QAction* m_unmask = nullptr;
    QAction* m_refresh = nullptr;

    int m_generation = 0;   // bumped on every reload; stale replies are dropped
    bool m_loading = false;
    bool m_busy = false;    // an action is running
    QString m_statusUnit;
};
