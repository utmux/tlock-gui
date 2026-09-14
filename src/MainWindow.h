#pragma once

#include "FileTask.h"

#include <QMainWindow>
#include <QPointer>
#include <QVector>

class HashWorker;
class QCheckBox;
class QCloseEvent;
class QDateTimeEdit;
class QDragEnterEvent;
class QDropEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QThread;
class QToolButton;
class TaskQueue;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct HashJob {
        QString path;
        QPointer<QThread> thread;
        QPointer<HashWorker> worker;
    };

    void buildUi();
    void connectUi();
    void loadSettings();
    void saveSettings();
    void locateTle();
    void setTlePath(const QString &path, bool persist);
    void testTle();
    void browseTle();
    void addFilesFromPaths(const QStringList &paths);
    void addFilesDialog();
    void removeSelectedFiles();
    void clearFiles();
    void refreshTable();
    void updateTableRow(int row, const FileTask &task);
    void updateOutputPaths(bool resetStatus = true);
    TaskMode selectedMode() const;
    bool sameOutputDirectory() const;
    void chooseOutputDirectory();
    void applyQuickTime(int kind, qint64 value);
    void startBatch();
    bool createBatchTasks(QVector<FileTask> *tasks, QString *errorMessage);
    bool checkBatchDiskSpace(const QVector<FileTask> &tasks, QString *errorMessage) const;
    void setUiRunning(bool running);
    void handleBatchFinished(bool stopped);
    void cancelScheduledShutdown();
    void showExistingTargetDialog(int taskIndex, const QString &targetPath);
    void appendLog(const QString &message);
    void showTableContextMenu(const QPoint &position);
    void startHashForRow(int row);
    void copyHashForRow(int row);
    int rowForPath(const QString &path) const;
    void stopHashJobs();
    void setAdvancedExpanded(bool expanded);
    void setLogExpanded(bool expanded);

    QVector<FileTask> m_files;
    QVector<HashJob> m_hashJobs;
    TaskQueue *m_queue = nullptr;
    QPointer<QProcess> m_testProcess;
    bool m_tleUsable = false;
    bool m_testTimedOut = false;
    bool m_closing = false;
    bool m_keepAwakeActive = false;
    bool m_shutdownRequestedForCurrentBatch = false;
    bool m_shutdownScheduled = false;

    QRadioButton *m_encryptRadio = nullptr;
    QRadioButton *m_decryptRadio = nullptr;
    QWidget *m_unlockPanel = nullptr;
    QDateTimeEdit *m_unlockEdit = nullptr;
    QLabel *m_timeZoneLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QRadioButton *m_sameDirectoryRadio = nullptr;
    QRadioButton *m_specificDirectoryRadio = nullptr;
    QLineEdit *m_outputDirectoryEdit = nullptr;
    QPushButton *m_outputBrowseButton = nullptr;
    QLineEdit *m_tlePathEdit = nullptr;
    QLabel *m_tleStatusLabel = nullptr;
    QPushButton *m_tleBrowseButton = nullptr;
    QPushButton *m_tleTestButton = nullptr;
    QLabel *m_currentFileLabel = nullptr;
    QLabel *m_currentProgressLabel = nullptr;
    QLabel *m_speedLabel = nullptr;
    QLabel *m_elapsedLabel = nullptr;
    QLabel *m_etaLabel = nullptr;
    QLabel *m_fileCountLabel = nullptr;
    QLabel *m_completedBytesLabel = nullptr;
    QProgressBar *m_totalProgressBar = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QCheckBox *m_keepAwakeCheck = nullptr;
    QCheckBox *m_shutdownAfterCheck = nullptr;
    QPushButton *m_cancelShutdownButton = nullptr;
    QToolButton *m_advancedToggle = nullptr;
    QWidget *m_advancedPanel = nullptr;
    QCheckBox *m_defaultNetworkCheck = nullptr;
    QLineEdit *m_networkEdit = nullptr;
    QLineEdit *m_chainEdit = nullptr;
    QToolButton *m_logToggle = nullptr;
    QPlainTextEdit *m_logEdit = nullptr;
};
