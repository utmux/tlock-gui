#include "MainWindow.h"

#include "HashWorker.h"
#include "PowerManager.h"
#include "TaskQueue.h"
#include "Utils.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTimeEdit>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {
constexpr qint64 diskSafetyMargin = 64LL * 1024LL * 1024LL;
const QString defaultNetwork = QStringLiteral("https://api.drand.sh/");
const QString defaultChain = QStringLiteral("52db9ba70e0cc0f6eaf7803dd07447a1f5477735fd3f661792ba94600c84e971");

qint64 saturatedAdd(qint64 left, qint64 right)
{
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right)
        return std::numeric_limits<qint64>::max();
    return left + right;
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_queue(new TaskQueue(this))
{
    qRegisterMetaType<FileTask>();
    setAcceptDrops(true);
    buildUi();
    connectUi();
    loadSettings();
    locateTle();
}

MainWindow::~MainWindow()
{
    if (m_testProcess && m_testProcess->state() != QProcess::NotRunning) {
        const QPointer<QProcess> process = m_testProcess;
        process->kill();
        if (process)
            process->waitForFinished(2000);
    }
    if (m_queue->isRunning())
        m_queue->forceShutdown();
    stopHashJobs();
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("TLockGUI - drand/tlock 大文件时间锁"));
    resize(980, 760);
    setMinimumSize(820, 620);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    auto *modeRow = new QHBoxLayout;
    auto *modeLabel = new QLabel(tr("模式："), central);
    m_encryptRadio = new QRadioButton(tr("加密"), central);
    m_decryptRadio = new QRadioButton(tr("解密"), central);
    m_encryptRadio->setChecked(true);
    modeRow->addWidget(modeLabel);
    modeRow->addWidget(m_encryptRadio);
    modeRow->addWidget(m_decryptRadio);
    modeRow->addStretch();
    modeRow->addWidget(new QLabel(tr("界面语言："), central));
    m_languageCombo = new QComboBox(central);
    m_languageCombo->addItem(tr("简体中文"), QStringLiteral("zh_CN"));
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    modeRow->addWidget(m_languageCombo);
    outer->addLayout(modeRow);

    m_unlockPanel = new QWidget(central);
    auto *unlockLayout = new QVBoxLayout(m_unlockPanel);
    unlockLayout->setContentsMargins(0, 0, 0, 0);
    auto *unlockRow = new QHBoxLayout;
    unlockRow->addWidget(new QLabel(tr("绝对解锁时间："), m_unlockPanel));
    m_unlockEdit = new QDateTimeEdit(m_unlockPanel);
    m_unlockEdit->setCalendarPopup(true);
    m_unlockEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_unlockEdit->setMinimumDateTime(QDateTime::currentDateTime().addSecs(-1));
    m_unlockEdit->setDateTime(QDateTime::currentDateTime().addDays(1));
    unlockRow->addWidget(m_unlockEdit);
    m_timeZoneLabel = new QLabel(m_unlockPanel);
    unlockRow->addWidget(m_timeZoneLabel);
    unlockRow->addStretch();
    unlockLayout->addLayout(unlockRow);

    auto *quickRow = new QHBoxLayout;
    const QList<QPair<QString, qint64>> quickTimes = {
        {tr("10 分钟后"), 10 * 60},
        {tr("1 小时后"), 60 * 60},
        {tr("1 天后"), 24 * 60 * 60},
        {tr("7 天后"), 7 * 24 * 60 * 60},
        {tr("30 天后"), 30LL * 24 * 60 * 60},
        {tr("180 天后"), 180LL * 24 * 60 * 60}
    };
    for (const auto &quickTime : quickTimes) {
        auto *button = new QPushButton(quickTime.first, m_unlockPanel);
        connect(button, &QPushButton::clicked, this,
                [this, seconds = quickTime.second]() { applyQuickTime(0, seconds); });
        quickRow->addWidget(button);
    }
    auto *yearButton = new QPushButton(tr("1 年后"), m_unlockPanel);
    connect(yearButton, &QPushButton::clicked, this,
            [this]() { applyQuickTime(1, 1); });
    quickRow->addWidget(yearButton);
    quickRow->addStretch();
    unlockLayout->addLayout(quickRow);
    outer->addWidget(m_unlockPanel);

    m_table = new QTableWidget(0, 7, central);
    m_table->setHorizontalHeaderLabels({
        tr("文件名"), tr("大小"), tr("完整路径"),
        tr("输出文件"), tr("状态"), tr("进度"),
        QStringLiteral("SHA-256")
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_table->setMinimumHeight(190);
    outer->addWidget(m_table, 1);

    auto *fileButtons = new QHBoxLayout;
    m_addButton = new QPushButton(tr("添加文件..."), central);
    m_removeButton = new QPushButton(tr("移除选中"), central);
    m_clearButton = new QPushButton(tr("清空列表"), central);
    fileButtons->addWidget(m_addButton);
    fileButtons->addWidget(m_removeButton);
    fileButtons->addWidget(m_clearButton);
    fileButtons->addStretch();
    outer->addLayout(fileButtons);

    auto *outputGroup = new QGroupBox(tr("输出设置"), central);
    auto *outputLayout = new QHBoxLayout(outputGroup);
    m_sameDirectoryRadio = new QRadioButton(tr("与源文件相同目录"), outputGroup);
    m_specificDirectoryRadio = new QRadioButton(tr("指定目录"), outputGroup);
    m_sameDirectoryRadio->setChecked(true);
    m_outputDirectoryEdit = new QLineEdit(outputGroup);
    m_outputDirectoryEdit->setPlaceholderText(tr("请选择已有且可写的输出目录"));
    m_outputBrowseButton = new QPushButton(tr("选择目录..."), outputGroup);
    outputLayout->addWidget(m_sameDirectoryRadio);
    outputLayout->addWidget(m_specificDirectoryRadio);
    outputLayout->addWidget(m_outputDirectoryEdit, 1);
    outputLayout->addWidget(m_outputBrowseButton);
    outer->addWidget(outputGroup);

    auto *tleGroup = new QGroupBox(tr("官方 tle.exe"), central);
    auto *tleLayout = new QHBoxLayout(tleGroup);
    m_tlePathEdit = new QLineEdit(tleGroup);
    m_tlePathEdit->setReadOnly(true);
    m_tleBrowseButton = new QPushButton(tr("浏览..."), tleGroup);
    m_tleTestButton = new QPushButton(tr("测试"), tleGroup);
    m_tleStatusLabel = new QLabel(tr("尚未验证"), tleGroup);
    tleLayout->addWidget(new QLabel(tr("tle.exe："), tleGroup));
    tleLayout->addWidget(m_tlePathEdit, 1);
    tleLayout->addWidget(m_tleBrowseButton);
    tleLayout->addWidget(m_tleTestButton);
    tleLayout->addWidget(m_tleStatusLabel);
    outer->addWidget(tleGroup);

    auto *statusGroup = new QGroupBox(tr("任务状态（进度为 .part 文件大小估算）"), central);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    auto *currentRow = new QHBoxLayout;
    m_currentFileLabel = new QLabel(tr("最近活动：—"), statusGroup);
    m_activeCountLabel = new QLabel(tr("运行中：0 / 1"), statusGroup);
    m_currentProgressLabel = new QLabel(tr("约进度：—"), statusGroup);
    m_speedLabel = new QLabel(tr("速度：—"), statusGroup);
    m_elapsedLabel = new QLabel(tr("已用：—"), statusGroup);
    m_etaLabel = new QLabel(tr("预计剩余：—"), statusGroup);
    currentRow->addWidget(m_currentFileLabel, 1);
    currentRow->addWidget(m_activeCountLabel);
    currentRow->addWidget(m_currentProgressLabel);
    currentRow->addWidget(m_speedLabel);
    currentRow->addWidget(m_elapsedLabel);
    currentRow->addWidget(m_etaLabel);
    statusLayout->addLayout(currentRow);
    auto *totalRow = new QHBoxLayout;
    m_fileCountLabel = new QLabel(tr("已完成 0 / 0"), statusGroup);
    m_completedBytesLabel = new QLabel(tr("已成功 0 B / 0 B"), statusGroup);
    m_totalProgressBar = new QProgressBar(statusGroup);
    m_totalProgressBar->setRange(0, 100);
    m_totalProgressBar->setValue(0);
    m_totalProgressBar->setFormat(tr("总进度约 %p%"));
    totalRow->addWidget(m_fileCountLabel);
    totalRow->addWidget(m_completedBytesLabel);
    totalRow->addWidget(m_totalProgressBar, 1);
    statusLayout->addLayout(totalRow);
    outer->addWidget(statusGroup);

    auto *powerRow = new QHBoxLayout;
    m_keepAwakeCheck = new QCheckBox(tr("处理期间阻止系统自动睡眠"), central);
    m_keepAwakeCheck->setChecked(true);
    m_keepAwakeCheck->setToolTip(tr("仅阻止 Windows 因空闲而自动睡眠；屏幕仍可关闭，合盖、低电量和手动睡眠不受影响。"));
    m_shutdownAfterCheck = new QCheckBox(tr("全部完成后关机"), central);
    m_shutdownAfterCheck->setToolTip(tr("仅在批次没有失败或取消时安排 60 秒后关机；开始任务前会再次确认。"));
    m_cancelShutdownButton = new QPushButton(tr("取消计划关机"), central);
    m_cancelShutdownButton->setEnabled(false);
    powerRow->addWidget(m_keepAwakeCheck);
    powerRow->addWidget(m_shutdownAfterCheck);
    powerRow->addWidget(m_cancelShutdownButton);
    powerRow->addStretch();
    outer->addLayout(powerRow);

    auto *actionRow = new QHBoxLayout;
    m_startButton = new QPushButton(tr("开始批处理"), central);
    m_startButton->setDefault(true);
    m_cancelButton = new QPushButton(tr("取消选中任务"), central);
    m_stopButton = new QPushButton(tr("停止全部"), central);
    m_cancelButton->setEnabled(false);
    m_stopButton->setEnabled(false);
    actionRow->addWidget(m_startButton);
    actionRow->addWidget(m_cancelButton);
    actionRow->addWidget(m_stopButton);
    actionRow->addStretch();
    outer->addLayout(actionRow);

    m_advancedToggle = new QToolButton(central);
    m_advancedToggle->setText(tr("高级设置"));
    m_advancedToggle->setCheckable(true);
    m_advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    outer->addWidget(m_advancedToggle);
    m_advancedPanel = new QWidget(central);
    auto *advancedLayout = new QFormLayout(m_advancedPanel);
    advancedLayout->setContentsMargins(20, 0, 0, 4);
    m_defaultNetworkCheck = new QCheckBox(tr("使用 tle 默认网络和 quicknet chain（不传 -n/-c）"), m_advancedPanel);
    m_defaultNetworkCheck->setChecked(true);
    m_parallelSpin = new QSpinBox(m_advancedPanel);
    m_parallelSpin->setRange(1, 8);
    m_parallelSpin->setValue(2);
    m_parallelSpin->setToolTip(tr("同时运行的 tle.exe 数量。默认 2；批次包含机械硬盘输入且数值大于 1 时，开始前会建议改为 1。"));
    m_networkEdit = new QLineEdit(defaultNetwork, m_advancedPanel);
    m_chainEdit = new QLineEdit(defaultChain, m_advancedPanel);
    advancedLayout->addRow(m_defaultNetworkCheck);
    advancedLayout->addRow(tr("并行任务数："), m_parallelSpin);
    advancedLayout->addRow(QStringLiteral("Network："), m_networkEdit);
    advancedLayout->addRow(QStringLiteral("Chain："), m_chainEdit);
    outer->addWidget(m_advancedPanel);

    m_logToggle = new QToolButton(central);
    m_logToggle->setText(tr("日志"));
    m_logToggle->setCheckable(true);
    m_logToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    outer->addWidget(m_logToggle);
    m_logEdit = new QPlainTextEdit(central);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMinimumHeight(130);
    outer->addWidget(m_logEdit);

    setCentralWidget(central);
    setAdvancedExpanded(false);
    setLogExpanded(true);
    m_timeZoneLabel->setText(tr("本机时区：%1").arg(Utils::timeZoneDescription()));
}

void MainWindow::connectUi()
{
    connect(m_addButton, &QPushButton::clicked, this, &MainWindow::addFilesDialog);
    connect(m_removeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedFiles);
    connect(m_clearButton, &QPushButton::clicked, this, &MainWindow::clearFiles);
    connect(m_outputBrowseButton, &QPushButton::clicked, this, &MainWindow::chooseOutputDirectory);
    connect(m_tleBrowseButton, &QPushButton::clicked, this, &MainWindow::browseTle);
    connect(m_tleTestButton, &QPushButton::clicked, this, &MainWindow::testTle);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startBatch);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelSelectedTask);
    connect(m_stopButton, &QPushButton::clicked, m_queue, &TaskQueue::stopAll);
    connect(m_cancelShutdownButton, &QPushButton::clicked,
            this, &MainWindow::cancelScheduledShutdown);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::showTableContextMenu);

    connect(m_encryptRadio, &QRadioButton::toggled, this, [this](bool checked) {
        m_unlockPanel->setEnabled(checked);
        updateOutputPaths();
    });
    connect(m_sameDirectoryRadio, &QRadioButton::toggled, this, [this](bool) {
        const bool specific = m_specificDirectoryRadio->isChecked();
        m_outputDirectoryEdit->setEnabled(specific);
        m_outputBrowseButton->setEnabled(specific);
        updateOutputPaths();
    });
    connect(m_outputDirectoryEdit, &QLineEdit::textChanged, this,
            [this]() { if (m_specificDirectoryRadio->isChecked()) updateOutputPaths(); });
    connect(m_unlockEdit, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime &dateTime) {
        m_timeZoneLabel->setText(tr("本机时区：%1").arg(Utils::timeZoneDescription(dateTime)));
    });
    connect(m_defaultNetworkCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_networkEdit->setEnabled(!checked);
        m_chainEdit->setEnabled(!checked);
    });
    connect(m_parallelSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int limit) {
        if (!m_queue->isRunning())
            m_activeCountLabel->setText(tr("运行中：%1 / %2").arg(0).arg(limit));
    });
    connect(m_advancedToggle, &QToolButton::toggled, this, &MainWindow::setAdvancedExpanded);
    connect(m_logToggle, &QToolButton::toggled, this, &MainWindow::setLogExpanded);
    connect(m_languageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
        const QString language = m_languageCombo->currentData().toString();
        if (language.isEmpty())
            return;
        QSettings settings;
        if (settings.value(QStringLiteral("ui/language"), QStringLiteral("zh_CN")).toString()
            == language) {
            return;
        }
        settings.setValue(QStringLiteral("ui/language"), language);
        if (!QCoreApplication::instance()->property("tlockgui.smokeTest").toBool()) {
            QMessageBox::information(this, tr("语言设置已保存"),
                                     tr("重新启动 TLockGUI 后将使用所选语言。"));
        }
    });

    connect(m_queue, &TaskQueue::runningChanged, this, &MainWindow::setUiRunning);
    connect(m_queue, &TaskQueue::logMessage, this, &MainWindow::appendLog);
    connect(m_queue, &TaskQueue::existingTargetFound,
            this, &MainWindow::showExistingTargetDialog);
    connect(m_queue, &TaskQueue::taskChanged, this, [this](int index, const FileTask &task) {
        if (index < 0 || index >= m_files.size())
            return;
        const QString sha = m_files[index].sha256;
        m_files[index] = task;
        if (m_files[index].sha256.isEmpty())
            m_files[index].sha256 = sha;
        updateTableRow(index, m_files[index]);
    });
    connect(m_queue, &TaskQueue::currentTaskChanged, this,
            [this](const QString &path, int, int) {
        m_currentFileLabel->setText(tr("最近活动：%1").arg(QFileInfo(path).fileName()));
    });
    connect(m_queue, &TaskQueue::activeCountChanged, this,
            [this](int active, int limit) {
        m_activeCountLabel->setText(tr("运行中：%1 / %2").arg(active).arg(limit));
    });
    connect(m_queue, &TaskQueue::currentProgress, this,
            [this](int, qint64, int percent, double speed, qint64 elapsed, qint64 eta) {
        m_currentProgressLabel->setText(tr("约进度：%1%").arg(percent));
        m_speedLabel->setText(tr("速度：%1").arg(Utils::formatRate(speed)));
        m_elapsedLabel->setText(tr("已用：%1").arg(Utils::formatDuration(elapsed)));
        m_etaLabel->setText(tr("预计剩余：%1").arg(Utils::formatDuration(eta)));
    });
    connect(m_queue, &TaskQueue::totalProgress, this,
            [this](int current, int total, qint64 completed, qint64 totalBytes, int percent) {
        m_fileCountLabel->setText(tr("已完成 %1 / %2").arg(current).arg(total));
        m_completedBytesLabel->setText(tr("已成功 %1 / %2")
                                           .arg(Utils::formatBytes(completed), Utils::formatBytes(totalBytes)));
        m_totalProgressBar->setValue(percent);
    });
    connect(m_queue, &TaskQueue::fatalError, this, [this](const QString &message) {
        QMessageBox::critical(this, tr("无法继续"), message);
    });
    connect(m_queue, &TaskQueue::batchFinished,
            this, &MainWindow::handleBatchFinished);
}

void MainWindow::loadSettings()
{
    QSettings settings;
    if (settings.contains(QStringLiteral("window/geometry")))
        restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());

    const QString outputDirectory = settings.value(QStringLiteral("output/lastDirectory")).toString();
    m_outputDirectoryEdit->setText(outputDirectory);
    const bool useSpecific = settings.value(QStringLiteral("output/useSpecific"), false).toBool();
    m_specificDirectoryRadio->setChecked(useSpecific);
    m_sameDirectoryRadio->setChecked(!useSpecific);

    m_defaultNetworkCheck->setChecked(settings.value(QStringLiteral("advanced/useDefaultNetwork"), true).toBool());
    m_networkEdit->setText(settings.value(QStringLiteral("advanced/network"), defaultNetwork).toString());
    m_chainEdit->setText(settings.value(QStringLiteral("advanced/chain"), defaultChain).toString());
    setAdvancedExpanded(settings.value(QStringLiteral("advanced/expanded"), false).toBool());
    setLogExpanded(settings.value(QStringLiteral("log/expanded"), true).toBool());
    m_keepAwakeCheck->setChecked(settings.value(QStringLiteral("power/keepAwake"), true).toBool());
    m_shutdownAfterCheck->setChecked(settings.value(QStringLiteral("power/shutdownAfter"), false).toBool());
    m_parallelSpin->setValue(qBound(1, settings.value(QStringLiteral("performance/maxParallel"), 2).toInt(), 8));
    m_activeCountLabel->setText(tr("运行中：%1 / %2").arg(0).arg(m_parallelSpin->value()));
    const QString language = settings.value(QStringLiteral("ui/language"), QStringLiteral("zh_CN")).toString();
    const int languageIndex = m_languageCombo->findData(language);
    m_languageCombo->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);

    const QDateTime savedTarget = settings.value(QStringLiteral("unlock/lastTarget")).toDateTime();
    if (savedTarget.isValid())
        m_unlockEdit->setDateTime(savedTarget);

    m_networkEdit->setEnabled(!m_defaultNetworkCheck->isChecked());
    m_chainEdit->setEnabled(!m_defaultNetworkCheck->isChecked());
    m_outputDirectoryEdit->setEnabled(m_specificDirectoryRadio->isChecked());
    m_outputBrowseButton->setEnabled(m_specificDirectoryRadio->isChecked());
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("output/lastDirectory"), m_outputDirectoryEdit->text());
    settings.setValue(QStringLiteral("output/useSpecific"), m_specificDirectoryRadio->isChecked());
    settings.setValue(QStringLiteral("advanced/useDefaultNetwork"), m_defaultNetworkCheck->isChecked());
    settings.setValue(QStringLiteral("advanced/network"), m_networkEdit->text());
    settings.setValue(QStringLiteral("advanced/chain"), m_chainEdit->text());
    settings.setValue(QStringLiteral("advanced/expanded"), m_advancedToggle->isChecked());
    settings.setValue(QStringLiteral("log/expanded"), m_logToggle->isChecked());
    settings.setValue(QStringLiteral("unlock/lastTarget"), m_unlockEdit->dateTime());
    settings.setValue(QStringLiteral("power/keepAwake"), m_keepAwakeCheck->isChecked());
    settings.setValue(QStringLiteral("power/shutdownAfter"), m_shutdownAfterCheck->isChecked());
    settings.setValue(QStringLiteral("performance/maxParallel"), m_parallelSpin->value());
    settings.setValue(QStringLiteral("ui/language"), m_languageCombo->currentData().toString());
}

void MainWindow::locateTle()
{
    QSettings settings;
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tle.exe")),
        settings.value(QStringLiteral("tle/path")).toString(),
        QStringLiteral("C:/tlock/tle.exe"),
        QStandardPaths::findExecutable(QStringLiteral("tle.exe"))
    };

    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile()) {
            setTlePath(QFileInfo(candidate).absoluteFilePath(), false);
            if (!QCoreApplication::instance()->property("tlockgui.smokeTest").toBool())
                QTimer::singleShot(0, this, &MainWindow::testTle);
            return;
        }
    }

    m_tleStatusLabel->setText(tr("未找到 tle.exe"));
    m_tleStatusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
    appendLog(tr("未按预设顺序找到 tle.exe，请手动选择。"));
    if (!QCoreApplication::instance()->property("tlockgui.smokeTest").toBool())
        QTimer::singleShot(0, this, &MainWindow::browseTle);
}

void MainWindow::setTlePath(const QString &path, bool persist)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    m_tlePathEdit->setText(QDir::toNativeSeparators(absolutePath));
    m_tleUsable = false;
    m_tleStatusLabel->setText(tr("尚未验证"));
    m_tleStatusLabel->setStyleSheet({});
    if (persist) {
        QSettings settings;
        settings.setValue(QStringLiteral("tle/path"), absolutePath);
    }
}

void MainWindow::testTle()
{
    if (m_testProcess && m_testProcess->state() != QProcess::NotRunning)
        return;
    const QString path = QDir::fromNativeSeparators(m_tlePathEdit->text());
    if (!QFileInfo(path).isFile()) {
        m_tleUsable = false;
        m_tleStatusLabel->setText(tr("tle.exe 不存在"));
        m_tleStatusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
        return;
    }

    auto *process = new QProcess(this);
    m_testProcess = process;
    m_tleUsable = false;
    m_testTimedOut = false;
    m_tleTestButton->setEnabled(false);
    m_tleStatusLabel->setText(tr("正在执行 --metadata..."));
    m_tleStatusLabel->setStyleSheet({});
    appendLog(tr("测试 tle.exe：\n程序：\n%1\n参数：\n--metadata").arg(path));
    process->setProgram(path);
    process->setArguments({QStringLiteral("--metadata")});
    process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_testProcess != process)
            return;
        m_tleUsable = false;
        m_tleStatusLabel->setText(tr("无法启动"));
        m_tleStatusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
        appendLog(tr("tle.exe 启动失败：%1").arg(process->errorString()));
        m_tleTestButton->setEnabled(true);
        m_testProcess = nullptr;
        process->deleteLater();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, path](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_testProcess != process)
            return;
        const QString output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
        m_tleUsable = !m_testTimedOut && exitStatus == QProcess::NormalExit && exitCode == 0;
        if (m_tleUsable) {
            m_tleStatusLabel->setText(tr("tle.exe 可用"));
            m_tleStatusLabel->setStyleSheet(QStringLiteral("color: #087f23;"));
            QSettings settings;
            settings.setValue(QStringLiteral("tle/path"), path);
            appendLog(tr("tle.exe --metadata 测试成功。\n%1").arg(output));
        } else {
            m_tleStatusLabel->setText(m_testTimedOut
                ? tr("测试超时") : tr("metadata 测试失败"));
            m_tleStatusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
            appendLog(tr("tle.exe --metadata 失败（退出代码 %1）。\n%2")
                          .arg(exitCode).arg(error.isEmpty() ? output : error));
        }
        m_tleTestButton->setEnabled(true);
        m_testProcess = nullptr;
        process->deleteLater();
    });
    process->start();
    QTimer::singleShot(15000, this, [this, process]() {
        if (m_testProcess == process && process->state() != QProcess::NotRunning) {
            m_testTimedOut = true;
            appendLog(tr("tle.exe --metadata 超过 15 秒，已终止测试。"));
            process->kill();
        }
    });
}

void MainWindow::browseTle()
{
    const QString initial = m_tlePathEdit->text().isEmpty()
        ? QStringLiteral("C:/tlock") : QFileInfo(m_tlePathEdit->text()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择官方 tle.exe"), initial,
        tr("tle.exe (tle.exe);;可执行文件 (*.exe);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    setTlePath(path, true);
    testTle();
}

void MainWindow::addFilesFromPaths(const QStringList &paths)
{
    if (m_queue->isRunning())
        return;
    QSet<QString> existing;
    for (const FileTask &task : m_files)
        existing.insert(Utils::normalizedPathKey(task.inputPath));

    int ignored = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            ++ignored;
            continue;
        }
        const QString absolutePath = info.absoluteFilePath();
        const QString key = Utils::normalizedPathKey(absolutePath);
        if (existing.contains(key)) {
            ++ignored;
            continue;
        }
        existing.insert(key);
        FileTask task;
        task.inputPath = absolutePath;
        task.inputSize = info.size();
        task.finalOutputPath = Utils::outputPathFor(
            absolutePath, selectedMode(), sameOutputDirectory(), m_outputDirectoryEdit->text());
        task.tempOutputPath = task.finalOutputPath + QStringLiteral(".part");
        m_files.append(task);
    }
    refreshTable();
    if (ignored > 0)
        appendLog(tr("已忽略 %1 个重复路径、目录或不存在的文件。").arg(ignored));
}

void MainWindow::addFilesDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("选择要处理的文件"), {}, tr("所有文件 (*)"));
    addFilesFromPaths(paths);
}

void MainWindow::removeSelectedFiles()
{
    if (m_queue->isRunning())
        return;
    QSet<int> rows;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
        rows.insert(index.row());
    QList<int> sortedRows = rows.values();
    std::sort(sortedRows.begin(), sortedRows.end(), std::greater<int>());
    for (int row : sortedRows) {
        if (row >= 0 && row < m_files.size())
            m_files.removeAt(row);
    }
    refreshTable();
}

void MainWindow::clearFiles()
{
    if (m_queue->isRunning())
        return;
    m_files.clear();
    refreshTable();
}

void MainWindow::refreshTable()
{
    m_table->setRowCount(m_files.size());
    for (int row = 0; row < m_files.size(); ++row)
        updateTableRow(row, m_files[row]);
}

void MainWindow::updateTableRow(int row, const FileTask &task)
{
    if (row < 0 || row >= m_table->rowCount())
        return;
    const QFileInfo info(task.inputPath);
    const QStringList values = {
        info.fileName(), Utils::formatBytes(task.inputSize), QDir::toNativeSeparators(task.inputPath),
        QDir::toNativeSeparators(task.finalOutputPath), Utils::taskStatusText(task.status)
    };
    for (int column = 0; column < values.size(); ++column) {
        if (!m_table->item(row, column))
            m_table->setItem(row, column, readOnlyItem(values[column]));
        else
            m_table->item(row, column)->setText(values[column]);
    }
    m_table->item(row, 2)->setToolTip(task.inputPath);
    m_table->item(row, 3)->setToolTip(task.finalOutputPath);
    m_table->item(row, 4)->setToolTip(task.errorMessage);

    auto *progress = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 5));
    if (!progress) {
        progress = new QProgressBar(m_table);
        progress->setRange(0, 100);
        progress->setTextVisible(true);
        progress->setFixedWidth(92);
        m_table->setCellWidget(row, 5, progress);
    }
    progress->setValue(task.progress);
    progress->setFormat(task.status == TaskStatus::Encrypting || task.status == TaskStatus::Decrypting
                            ? tr("约 %p%") : QStringLiteral("%p%"));

    if (!m_table->item(row, 6))
        m_table->setItem(row, 6, readOnlyItem({}));
    QTableWidgetItem *hashItem = m_table->item(row, 6);
    if (!task.sha256.isEmpty()) {
        hashItem->setText(task.sha256.left(12) + QStringLiteral("…"));
        hashItem->setToolTip(task.sha256);
    } else if (!hashItem->text().startsWith(tr("计算中"))) {
        hashItem->setText(tr("未计算"));
        hashItem->setToolTip(tr("右键选择“计算 SHA-256”"));
    }
}

void MainWindow::updateOutputPaths(bool resetStatus)
{
    if (m_queue->isRunning())
        return;
    for (FileTask &task : m_files) {
        task.finalOutputPath = Utils::outputPathFor(
            task.inputPath, selectedMode(), sameOutputDirectory(), m_outputDirectoryEdit->text());
        task.tempOutputPath = task.finalOutputPath + QStringLiteral(".part");
        if (resetStatus) {
            task.status = TaskStatus::Pending;
            task.progress = 0;
            task.outputBytes = 0;
            task.errorMessage.clear();
        }
    }
    refreshTable();
}

TaskMode MainWindow::selectedMode() const
{
    return m_encryptRadio->isChecked() ? TaskMode::Encrypt : TaskMode::Decrypt;
}

bool MainWindow::sameOutputDirectory() const
{
    return m_sameDirectoryRadio->isChecked();
}

void MainWindow::chooseOutputDirectory()
{
    const QString initial = m_outputDirectoryEdit->text().isEmpty()
        ? QDir::homePath() : m_outputDirectoryEdit->text();
    const QString path = QFileDialog::getExistingDirectory(
        this, tr("选择输出目录"), initial,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty())
        return;
    m_specificDirectoryRadio->setChecked(true);
    m_outputDirectoryEdit->setText(QDir::toNativeSeparators(path));
}

void MainWindow::applyQuickTime(int kind, qint64 value)
{
    const QDateTime now = QDateTime::currentDateTime();
    m_unlockEdit->setDateTime(kind == 1 ? now.addYears(static_cast<int>(value)) : now.addSecs(value));
}

void MainWindow::startBatch()
{
    if (m_queue->isRunning())
        return;
    if (m_shutdownScheduled) {
        QMessageBox::warning(this, tr("已安排系统关机"),
                             tr("请先点击“取消计划关机”，再开始新的批处理。"));
        return;
    }
    if (!m_tleUsable) {
        QMessageBox::warning(this, tr("tle.exe 尚不可用"),
                             tr("请先使用“测试”按钮确认官方 tle.exe 的 --metadata 测试成功。"));
        return;
    }
    if (selectedMode() == TaskMode::Encrypt
        && QDateTime::currentDateTime().secsTo(m_unlockEdit->dateTime()) <= 0) {
        QMessageBox::warning(this, tr("解锁时间无效"),
                             tr("目标解锁时间必须晚于当前时间。"));
        return;
    }
    if (!m_defaultNetworkCheck->isChecked()
        && (m_networkEdit->text().trimmed().isEmpty() || m_chainEdit->text().trimmed().isEmpty())) {
        QMessageBox::warning(this, tr("高级设置无效"),
                             tr("自定义网络模式下 Network 和 Chain 都不能为空。"));
        return;
    }

    QVector<FileTask> tasks;
    QString error;
    if (!createBatchTasks(&tasks, &error)) {
        QMessageBox::warning(this, tr("无法开始"), error);
        return;
    }
    if (!checkBatchDiskSpace(tasks, &error)) {
        QMessageBox::critical(this, tr("磁盘空间不足"), error);
        return;
    }

    int effectiveParallelism = m_parallelSpin->value();
    if (!chooseParallelismForStorage(tasks, &effectiveParallelism))
        return;

    if (m_shutdownAfterCheck->isChecked()) {
        const auto answer = QMessageBox::warning(
            this, tr("确认完成后关机"),
            tr("本批次全部处理完成且没有失败或取消时，Windows 将在 60 秒后关机。\n"
               "关机前可使用“取消计划关机”按钮撤销。\n"
               "请先保存其他程序中未保存的内容；倒计时结束时 Windows 可能强制关闭应用。\n\n"
               "是否按此设置开始？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_shutdownRequestedForCurrentBatch = m_shutdownAfterCheck->isChecked();

    m_files = tasks;
    refreshTable();
    BatchSettings settings;
    settings.tlePath = QDir::fromNativeSeparators(m_tlePathEdit->text());
    settings.mode = selectedMode();
    settings.unlockTarget = m_unlockEdit->dateTime();
    settings.useDefaultNetwork = m_defaultNetworkCheck->isChecked();
    settings.network = m_networkEdit->text().trimmed();
    settings.chain = m_chainEdit->text().trimmed();
    settings.maxParallelTasks = effectiveParallelism;
    appendLog(tr("开始并行批处理，共 %1 个文件，并发上限 %2，总输入大小 %3。")
                  .arg(tasks.size()).arg(settings.maxParallelTasks).arg(Utils::formatBytes([&tasks]() {
                      qint64 total = 0;
                      for (const FileTask &task : tasks)
                          total = saturatedAdd(total, task.inputSize);
                      return total;
                  }())));
    m_queue->start(tasks, settings);
}

bool MainWindow::chooseParallelismForStorage(const QVector<FileTask> &tasks, int *parallelism)
{
    if (!parallelism || *parallelism <= 1 || tasks.size() <= 1)
        return true;

    QHash<QString, int> taskCountsByVolume;
    QHash<QString, QString> displayRootByVolume;
    QStringList volumeOrder;
    QStringList rotationalVolumes;
    for (const FileTask &task : tasks) {
        QString root;
        if (Utils::storageMediaTypeForPath(task.inputPath, &root)
            != Utils::StorageMediaType::Rotational || root.isEmpty()) {
            continue;
        }
        const QString key = root.toCaseFolded();
        if (!taskCountsByVolume.contains(key)) {
            volumeOrder.append(key);
            displayRootByVolume.insert(key, root);
        }
        taskCountsByVolume[key] += 1;
    }
    for (const QString &key : volumeOrder) {
        if (taskCountsByVolume.value(key) > 1)
            rotationalVolumes.append(displayRootByVolume.value(key));
    }
    if (rotationalVolumes.isEmpty())
        return true;

    QMessageBox box(QMessageBox::Warning, tr("检测到机械硬盘输入"),
                    tr("Windows 检测到以下输入盘为机械硬盘：\n%1")
                        .arg(rotationalVolumes.join(tr("、"))),
                    QMessageBox::NoButton, this);
    box.setInformativeText(
        tr("当前并发为 %1。多个 tle.exe 同时读取同一机械硬盘会导致磁头来回寻道，"
           "总速度通常更慢。建议本批次改为并发 1；SSD/NVMe 默认仍为 2。")
            .arg(*parallelism));
    QPushButton *useOne = box.addButton(tr("使用 1（推荐）"), QMessageBox::AcceptRole);
    QPushButton *keepCurrent = box.addButton(tr("保持 %1").arg(*parallelism), QMessageBox::ActionRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(useOne);
    box.setEscapeButton(cancel);
    box.exec();

    if (box.clickedButton() == cancel)
        return false;
    if (box.clickedButton() == useOne) {
        *parallelism = 1;
        appendLog(tr("检测到机械硬盘输入：%1。本批次并发已调整为 1。")
                      .arg(rotationalVolumes.join(tr("、"))));
    } else if (box.clickedButton() == keepCurrent) {
        appendLog(tr("检测到机械硬盘输入：%1。用户选择保留并发 %2。")
                      .arg(rotationalVolumes.join(tr("、")))
                      .arg(*parallelism));
    }
    return true;
}

void MainWindow::cancelSelectedTask()
{
    if (!m_queue->isRunning())
        return;

    QSet<int> rows;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
        rows.insert(index.row());

    int cancelled = 0;
    for (int row : rows) {
        if (row < 0 || row >= m_files.size())
            continue;
        const TaskStatus status = m_files[row].status;
        if (status == TaskStatus::Pending || status == TaskStatus::Encrypting
            || status == TaskStatus::Decrypting) {
            m_queue->cancelTask(row);
            ++cancelled;
        }
    }
    if (cancelled == 0)
        m_queue->cancelCurrent();
}

bool MainWindow::createBatchTasks(QVector<FileTask> *tasks, QString *errorMessage)
{
    if (m_files.isEmpty()) {
        *errorMessage = tr("请先添加至少一个文件。");
        return false;
    }

    QString outputDirectory;
    if (!sameOutputDirectory()) {
        outputDirectory = QDir::fromNativeSeparators(m_outputDirectoryEdit->text().trimmed());
        QString directoryError;
        if (!Utils::isPathWritableDirectory(outputDirectory, &directoryError)) {
            *errorMessage = directoryError;
            return false;
        }
    }

    QSet<QString> outputs;
    QSet<QString> checkedDirectories;
    tasks->clear();
    tasks->reserve(m_files.size());
    for (const FileTask &source : m_files) {
        const QFileInfo inputInfo(source.inputPath);
        if (!inputInfo.exists() || !inputInfo.isFile()) {
            *errorMessage = tr("输入文件不存在或不是普通文件：\n%1").arg(source.inputPath);
            return false;
        }
        if (inputInfo.size() <= 0) {
            *errorMessage = tr("输入文件为空：\n%1").arg(source.inputPath);
            return false;
        }

        FileTask task = source;
        task.inputSize = inputInfo.size();
        task.finalOutputPath = Utils::outputPathFor(
            task.inputPath, selectedMode(), sameOutputDirectory(), outputDirectory);
        task.tempOutputPath = task.finalOutputPath + QStringLiteral(".part");
        task.status = TaskStatus::Pending;
        task.progress = 0;
        task.outputBytes = 0;
        task.errorMessage.clear();

        const QString inputKey = Utils::normalizedPathKey(task.inputPath);
        const QString outputKey = Utils::normalizedPathKey(task.finalOutputPath);
        if (inputKey == outputKey) {
            *errorMessage = tr("输出文件不能与输入文件相同：\n%1").arg(task.inputPath);
            return false;
        }
        if (outputs.contains(outputKey)) {
            *errorMessage = tr("多个输入文件会生成同一个输出文件：\n%1\n"
                               "请改用源文件目录或调整文件名。")
                .arg(task.finalOutputPath);
            return false;
        }
        outputs.insert(outputKey);

        const QString directory = QFileInfo(task.finalOutputPath).absolutePath();
        const QString directoryKey = Utils::normalizedPathKey(directory);
        if (!checkedDirectories.contains(directoryKey)) {
            checkedDirectories.insert(directoryKey);
            QString directoryError;
            if (!Utils::isPathWritableDirectory(directory, &directoryError)) {
                *errorMessage = directoryError;
                return false;
            }
        }
        tasks->append(task);
    }
    return true;
}

bool MainWindow::checkBatchDiskSpace(const QVector<FileTask> &tasks, QString *errorMessage) const
{
    QHash<QString, qint64> requiredByVolume;
    QHash<QString, qint64> availableByVolume;
    for (const FileTask &task : tasks) {
        const QString directory = QFileInfo(task.finalOutputPath).absolutePath();
        const QStorageInfo storage(directory);
        if (!storage.isValid() || !storage.isReady())
            continue;
        const QString volumeKey = Utils::normalizedPathKey(storage.rootPath());
        requiredByVolume[volumeKey] = saturatedAdd(
            requiredByVolume.value(volumeKey), saturatedAdd(task.inputSize, diskSafetyMargin));
        availableByVolume[volumeKey] = storage.bytesAvailable();
    }

    for (auto iterator = requiredByVolume.constBegin(); iterator != requiredByVolume.constEnd(); ++iterator) {
        const qint64 available = availableByVolume.value(iterator.key());
        if (available <= iterator.value()) {
            *errorMessage = tr("输出卷 %1 空间明显不足。\n批量保守估算需要：%2\n当前可用：%3\n"
                               "估算包含每个文件 64 MiB 安全余量。")
                .arg(iterator.key(), Utils::formatBytes(iterator.value()), Utils::formatBytes(available));
            return false;
        }
    }
    return true;
}

void MainWindow::setUiRunning(bool running)
{
    if (running && m_keepAwakeCheck->isChecked() && !m_keepAwakeActive) {
        QString error;
        m_keepAwakeActive = PowerManager::setKeepAwake(true, &error);
        if (m_keepAwakeActive)
            appendLog(tr("已请求 Windows 在批处理期间保持唤醒（允许屏幕关闭）。"));
        else
            appendLog(tr("警告：无法阻止系统自动睡眠：%1").arg(error));
    } else if (!running && m_keepAwakeActive) {
        QString error;
        if (!PowerManager::setKeepAwake(false, &error))
            appendLog(tr("警告：恢复 Windows 默认睡眠策略失败：%1").arg(error));
        else
            appendLog(tr("已恢复 Windows 默认睡眠策略。"));
        m_keepAwakeActive = false;
    }

    m_addButton->setEnabled(!running);
    m_removeButton->setEnabled(!running);
    m_clearButton->setEnabled(!running);
    m_encryptRadio->setEnabled(!running);
    m_decryptRadio->setEnabled(!running);
    m_unlockEdit->setEnabled(!running);
    m_sameDirectoryRadio->setEnabled(!running);
    m_specificDirectoryRadio->setEnabled(!running);
    m_outputDirectoryEdit->setEnabled(!running && m_specificDirectoryRadio->isChecked());
    m_outputBrowseButton->setEnabled(!running && m_specificDirectoryRadio->isChecked());
    m_tleBrowseButton->setEnabled(!running);
    m_tleTestButton->setEnabled(!running && !m_testProcess);
    m_defaultNetworkCheck->setEnabled(!running);
    m_parallelSpin->setEnabled(!running);
    m_languageCombo->setEnabled(!running);
    m_networkEdit->setEnabled(!running && !m_defaultNetworkCheck->isChecked());
    m_chainEdit->setEnabled(!running && !m_defaultNetworkCheck->isChecked());
    m_startButton->setEnabled(!running);
    m_cancelButton->setEnabled(running);
    m_stopButton->setEnabled(running);
    m_keepAwakeCheck->setEnabled(!running && !m_shutdownScheduled);
    m_shutdownAfterCheck->setEnabled(!running && !m_shutdownScheduled);
    m_cancelShutdownButton->setEnabled(!running && m_shutdownScheduled);
}

void MainWindow::handleBatchFinished(bool stopped)
{
    int success = 0;
    int failed = 0;
    int skipped = 0;
    int cancelled = 0;
    for (const FileTask &task : m_files) {
        success += task.status == TaskStatus::Success ? 1 : 0;
        failed += task.status == TaskStatus::Failed ? 1 : 0;
        skipped += task.status == TaskStatus::Skipped ? 1 : 0;
        cancelled += task.status == TaskStatus::Cancelled ? 1 : 0;
    }
    appendLog(tr("批处理结束：成功 %1，失败 %2，跳过 %3，取消 %4。")
                  .arg(success).arg(failed).arg(skipped).arg(cancelled));

    if (m_closing || QCoreApplication::instance()->property("tlockgui.smokeTest").toBool()) {
        m_shutdownRequestedForCurrentBatch = false;
        return;
    }

    QString message = tr("批处理已%1。\n成功：%2\n失败：%3\n跳过：%4\n取消：%5")
        .arg(stopped ? tr("停止") : tr("完成"))
        .arg(success).arg(failed).arg(skipped).arg(cancelled);
    QStringList failureDetails;
    for (const FileTask &task : m_files) {
        if (task.status != TaskStatus::Failed || task.errorMessage.trimmed().isEmpty())
            continue;
        failureDetails.append(QStringLiteral("• %1\n  %2")
                                  .arg(QFileInfo(task.inputPath).fileName(), task.errorMessage.trimmed()));
        if (failureDetails.size() == 3)
            break;
    }
    if (!failureDetails.isEmpty()) {
        message += tr("\n\n失败详情（最多显示 3 项）：\n%1")
                       .arg(failureDetails.join(QLatin1Char('\n')));
        if (failed > failureDetails.size()) {
            message += tr("\n另有 %1 个失败任务，请查看日志或将鼠标停在表格“状态”列上查看详情。")
                           .arg(failed - failureDetails.size());
        }
    }
    if (selectedMode() == TaskMode::Encrypt && success > 0) {
        message += tr("\n\n请确认密文已经正确保存和备份后，再自行决定是否删除原始文件。"
                      "TLockGUI 不会删除任何输入文件。");
    }

    if (m_shutdownRequestedForCurrentBatch) {
        if (!stopped && failed == 0 && cancelled == 0) {
            QString error;
            if (PowerManager::scheduleShutdown(60, &error)) {
                m_shutdownScheduled = true;
                m_cancelShutdownButton->setEnabled(true);
                m_keepAwakeCheck->setEnabled(false);
                m_shutdownAfterCheck->setEnabled(false);
                m_startButton->setEnabled(false);
                appendLog(tr("已安排 Windows 在 60 秒后关机。可点击“取消计划关机”撤销。"));
                message += tr("\n\n已安排 Windows 在 60 秒后关机。需要时请点击“取消计划关机”。");
            } else {
                appendLog(tr("自动关机安排失败：%1").arg(error));
                message += tr("\n\n未能安排自动关机：%1").arg(error);
            }
        } else {
            appendLog(tr("由于批次被停止、存在失败或取消任务，未执行自动关机。"));
            message += tr("\n\n由于存在失败、取消或手动停止，未执行自动关机。");
        }
    }
    m_shutdownRequestedForCurrentBatch = false;
    QMessageBox::information(this, QStringLiteral("TLockGUI"), message);
}

void MainWindow::cancelScheduledShutdown()
{
    if (!m_shutdownScheduled)
        return;
    QString error;
    if (!PowerManager::cancelShutdown(&error)) {
        QMessageBox::critical(this, tr("取消关机失败"), error);
        appendLog(tr("取消计划关机失败：%1").arg(error));
        return;
    }
    m_shutdownScheduled = false;
    m_cancelShutdownButton->setEnabled(false);
    m_keepAwakeCheck->setEnabled(true);
    m_shutdownAfterCheck->setEnabled(true);
    m_startButton->setEnabled(true);
    appendLog(tr("已取消计划关机。"));
}

void MainWindow::showExistingTargetDialog(int, const QString &targetPath)
{
    QMessageBox box(QMessageBox::Warning, tr("目标文件已存在"),
                    tr("目标文件已存在：\n%1\n\n"
                       "TLockGUI 不会预先删除它；选择覆盖时，将在新 .part 完成后进行安全替换。")
                        .arg(QDir::toNativeSeparators(targetPath)),
                    QMessageBox::NoButton, this);
    QPushButton *overwrite = box.addButton(tr("覆盖"), QMessageBox::AcceptRole);
    QPushButton *skip = box.addButton(tr("跳过"), QMessageBox::RejectRole);
    QPushButton *overwriteAll = box.addButton(tr("全部覆盖"), QMessageBox::YesRole);
    QPushButton *skipAll = box.addButton(tr("全部跳过"), QMessageBox::NoRole);
    QPushButton *cancelAll = box.addButton(tr("取消整个任务"), QMessageBox::DestructiveRole);
    box.exec();

    if (box.clickedButton() == overwrite)
        m_queue->resolveExistingTarget(TaskQueue::ExistingTargetDecision::Overwrite);
    else if (box.clickedButton() == skip)
        m_queue->resolveExistingTarget(TaskQueue::ExistingTargetDecision::Skip);
    else if (box.clickedButton() == overwriteAll)
        m_queue->resolveExistingTarget(TaskQueue::ExistingTargetDecision::OverwriteAll);
    else if (box.clickedButton() == skipAll)
        m_queue->resolveExistingTarget(TaskQueue::ExistingTargetDecision::SkipAll);
    else if (box.clickedButton() == cancelAll || !box.clickedButton())
        m_queue->resolveExistingTarget(TaskQueue::ExistingTargetDecision::CancelAll);
}

void MainWindow::appendLog(const QString &message)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    QString formatted = message;
    formatted.replace(QLatin1Char('\r'), QString());
    const QStringList lines = formatted.split(QLatin1Char('\n'));
    for (const QString &line : lines)
        m_logEdit->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, line));
}

void MainWindow::showTableContextMenu(const QPoint &position)
{
    const int row = m_table->rowAt(position.y());
    if (row < 0 || row >= m_files.size())
        return;
    QMenu menu(this);
    QAction *calculate = menu.addAction(tr("计算 SHA-256"));
    QAction *copy = menu.addAction(tr("复制 SHA-256"));
    calculate->setEnabled(rowForPath(m_files[row].inputPath) >= 0 && !m_queue->isRunning());
    bool alreadyRunning = false;
    for (const HashJob &job : m_hashJobs)
        alreadyRunning = alreadyRunning || Utils::normalizedPathKey(job.path) == Utils::normalizedPathKey(m_files[row].inputPath);
    calculate->setEnabled(calculate->isEnabled() && !alreadyRunning);
    copy->setEnabled(!m_files[row].sha256.isEmpty());
    QAction *selected = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (selected == calculate)
        startHashForRow(row);
    else if (selected == copy)
        copyHashForRow(row);
}

void MainWindow::startHashForRow(int row)
{
    if (row < 0 || row >= m_files.size() || m_queue->isRunning())
        return;
    const QString path = m_files[row].inputPath;
    for (const HashJob &job : m_hashJobs) {
        if (Utils::normalizedPathKey(job.path) == Utils::normalizedPathKey(path))
            return;
    }

    auto *thread = new QThread(this);
    auto *worker = new HashWorker;
    worker->moveToThread(thread);
    m_hashJobs.append({path, thread, worker});
    m_table->item(row, 6)->setText(tr("计算中 0%"));
    appendLog(tr("开始后台计算 SHA-256：%1").arg(path));

    connect(thread, &QThread::started, worker, [worker, path]() { worker->calculate(path); });
    connect(worker, &HashWorker::progress, this, [this](const QString &workerPath, int percent) {
        const int currentRow = rowForPath(workerPath);
        if (currentRow >= 0 && m_table->item(currentRow, 6))
            m_table->item(currentRow, 6)->setText(tr("计算中 %1%").arg(percent));
    });
    connect(worker, &HashWorker::finished, this,
            [this](const QString &workerPath, const QString &digest, const QString &error) {
        const int currentRow = rowForPath(workerPath);
        if (currentRow >= 0) {
            if (!digest.isEmpty()) {
                m_files[currentRow].sha256 = digest;
                updateTableRow(currentRow, m_files[currentRow]);
                appendLog(tr("SHA-256 完成：%1\n%2").arg(workerPath, digest));
            } else {
                m_table->item(currentRow, 6)->setText(tr("计算失败"));
                m_table->item(currentRow, 6)->setToolTip(error);
                appendLog(tr("SHA-256 未完成：%1\n%2").arg(workerPath, error));
            }
        }
    });
    connect(worker, &HashWorker::finished, thread, &QThread::quit);
    connect(worker, &HashWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread, path]() {
        for (int index = m_hashJobs.size() - 1; index >= 0; --index) {
            if (m_hashJobs[index].thread == thread)
                m_hashJobs.removeAt(index);
        }
        thread->deleteLater();
        Q_UNUSED(path);
    });
    thread->start();
}

void MainWindow::copyHashForRow(int row)
{
    if (row < 0 || row >= m_files.size() || m_files[row].sha256.isEmpty())
        return;
    QApplication::clipboard()->setText(m_files[row].sha256);
    appendLog(tr("已复制 SHA-256：%1").arg(QFileInfo(m_files[row].inputPath).fileName()));
}

int MainWindow::rowForPath(const QString &path) const
{
    const QString key = Utils::normalizedPathKey(path);
    for (int row = 0; row < m_files.size(); ++row) {
        if (Utils::normalizedPathKey(m_files[row].inputPath) == key)
            return row;
    }
    return -1;
}

void MainWindow::stopHashJobs()
{
    const QVector<HashJob> jobs = m_hashJobs;
    for (const HashJob &job : jobs) {
        if (job.worker)
            job.worker->cancel();
    }
    for (const HashJob &job : jobs) {
        if (job.thread && job.thread->isRunning()) {
            job.thread->quit();
            job.thread->wait();
        }
    }
    m_hashJobs.clear();
}

void MainWindow::setAdvancedExpanded(bool expanded)
{
    m_advancedToggle->setChecked(expanded);
    m_advancedToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    m_advancedPanel->setVisible(expanded);
}

void MainWindow::setLogExpanded(bool expanded)
{
    m_logToggle->setChecked(expanded);
    m_logToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    m_logEdit->setVisible(expanded);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_queue->isRunning() || !event->mimeData()->hasUrls())
        return;
    bool hasLocalFile = false;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            hasLocalFile = true;
            break;
        }
    }
    if (hasLocalFile)
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    addFilesFromPaths(paths);
    event->acceptProposedAction();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete && m_table->hasFocus() && !m_queue->isRunning()) {
        removeSelectedFiles();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_queue->isRunning()) {
        const auto answer = QMessageBox::warning(
            this, tr("当前仍在处理大文件"),
            tr("强制退出会终止所有运行中的任务并删除本次生成的 .part，但不会删除原始文件。\n\n是否退出？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_closing = true;
        m_queue->forceShutdown();
    }

    m_closing = true;
    if (m_testProcess && m_testProcess->state() != QProcess::NotRunning) {
        const QPointer<QProcess> process = m_testProcess;
        process->kill();
        if (process)
            process->waitForFinished(2000);
    }
    stopHashJobs();
    if (m_keepAwakeActive) {
        PowerManager::setKeepAwake(false);
        m_keepAwakeActive = false;
    }
    saveSettings();
    event->accept();
}
