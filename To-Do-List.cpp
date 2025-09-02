#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <gdiplus.h>
#include <fstream>
#include <ctime>
#include <algorithm>
#include <locale>
#include <shlobj.h>
#include <objbase.h>
#include <shlguid.h>

// 定义Unicode宏
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

using namespace Gdiplus;

// 控件ID
#define ID_BUTTON_ADD       1001
#define ID_BUTTON_CLEAR     1002
#define ID_SCROLL           1003
#define ID_EDIT_TASK        1004
#define ID_EDIT_DESC        1005
#define ID_BUTTON_SAVE      1006
#define ID_BUTTON_CANCEL    1007
#define ID_CHECK_AUTOSTART  1008

// 托盘图标相关
#define WM_TRAYICON         (WM_USER + 1)
#define ID_TRAY_SHOW        2001
#define ID_TRAY_EXIT        2002

// 全局变量
HWND g_hWnd = NULL;
HWND g_hBtnAdd = NULL;
HWND g_hBtnSave = NULL;
HWND g_hBtnCancel = NULL;
HWND g_hEditTask = NULL;
HWND g_hEditDesc = NULL;
HWND g_hScrollWnd = NULL;
HWND g_hCheckAutoStart = NULL;
std::string g_statusText = "欢迎使用待办清单管理工具";
std::mutex g_dataMutex;
NOTIFYICONDATAW g_trayIcon = {0};
bool g_isTrayIconAdded = false;

// 编辑状态
bool g_isEditing = false;
int g_editingTaskId = -1;

// 滚动相关 - 分别为待完成和已完成区域
int g_pendingScrollPos = 0;
int g_completedScrollPos = 0;
int g_maxPendingScrollPos = 0;
int g_maxCompletedScrollPos = 0;
int g_currentScrollArea = 0; // 0=待完成区域, 1=已完成区域

// 窗口吸附相关
bool g_isSnapped = false;           // 是否已吸附
bool g_isHidden = false;            // 是否已隐藏
int g_snapSide = 0;                 // 吸附边：0=无, 1=左, 2=右, 3=上, 4=下
RECT g_normalRect = {0};            // 正常状态下的窗口位置
RECT g_hiddenRect = {0};            // 隐藏状态下的窗口位置
const int SNAP_DISTANCE = 30;      // 吸附距离，增加灵敏度
const int HIDE_WIDTH = 10;          // 隐藏时露出的宽度
UINT_PTR g_hideTimer = 0;           // 隐藏定时器
const int HIDE_DELAY = 1000;        // 隐藏延迟（毫秒）
HHOOK g_mouseHook = NULL;           // 全局鼠标钩子

// 任务状态
enum TaskStatus {
    TASK_PENDING,       // 待完成
    TASK_IN_PROGRESS,   // 进行中
    TASK_COMPLETED,     // 已完成
    TASK_CANCELLED      // 已取消
};

// 任务优先级
enum TaskPriority {
    PRIORITY_LOW,       // 低优先级
    PRIORITY_NORMAL,    // 普通优先级
    PRIORITY_HIGH,      // 高优先级
    PRIORITY_URGENT     // 紧急
};

// 任务项结构
struct TaskItem {
    int id;                         // 任务ID
    std::string title;              // 任务标题
    std::string description;        // 任务描述
    TaskStatus status;              // 任务状态
    TaskPriority priority;          // 优先级
    std::string createTime;         // 创建时间
    std::string completeTime;       // 完成时间

    TaskItem() : id(0), status(TASK_PENDING), priority(PRIORITY_NORMAL) {
        // 获取当前时间作为创建时间
        time_t now = time(0);
        char timeStr[100];
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        createTime = timeStr;
    }
};

// 待办清单管理器
struct TodoManager {
    std::vector<TaskItem> tasks;
    int nextId = 1;

    int addTask(const std::string& title, const std::string& desc = "", TaskPriority priority = PRIORITY_NORMAL) {
        TaskItem task;
        task.id = nextId++;
        task.title = title;
        task.description = desc;
        task.priority = priority;
        tasks.push_back(task);
        return task.id;
    }

    bool removeTask(int id) {
        auto it = std::find_if(tasks.begin(), tasks.end(), [id](const TaskItem& task) {
            return task.id == id;
        });
        if (it != tasks.end()) {
            tasks.erase(it);
            return true;
        }
        return false;
    }

    bool updateTaskStatus(int id, TaskStatus status) {
        auto it = std::find_if(tasks.begin(), tasks.end(), [id](TaskItem& task) {
            return task.id == id;
        });
        if (it != tasks.end()) {
            it->status = status;
            if (status == TASK_COMPLETED) {
                time_t now = time(0);
                char timeStr[100];
                struct tm timeinfo;
                localtime_s(&timeinfo, &now);
                strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
                it->completeTime = timeStr;
            }
            return true;
        }
        return false;
    }

    std::vector<TaskItem> getTasksByStatus(TaskStatus status) {
        std::vector<TaskItem> result;
        for (const auto& task : tasks) {
            if (task.status == status) {
                result.push_back(task);
            }
        }
        return result;
    }
};

TodoManager g_todoManager;

// 函数声明
void DrawRoundedRect(Graphics& graphics, Brush& brush, Pen& pen, int x, int y, int width, int height, int radius);
std::wstring StringToWString(const std::string& str);
std::string WStringToString(const std::wstring& wstr);
std::wstring ConfigStringToWString(const std::string& str);
std::string EscapeJsonString(const std::string& str);
std::string UnescapeJsonString(const std::string& str);
void CancelEdit();
void AddNewTask();
void StartEditTask(int taskId);
void SaveEditTask();
void CheckWindowSnap(HWND hwnd);
void HideWindowToEdge(HWND hwnd);
void ShowWindowFromEdge(HWND hwnd);
void HandleMouseMove(HWND hwnd);
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);
std::wstring GetStartupFolderPath();
bool CreateStartupShortcut();
bool RemoveStartupShortcut();
bool IsAutoStartEnabled();
void SetAutoStart(bool enable);
void SaveWindowConfig();
bool LoadWindowConfig();
void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hwnd);


// JSON配置文件路径
const std::string CONFIG_FILE = "todo_list_config.json";
const std::string WINDOW_CONFIG_FILE = "window_config.json";

// 超链接区域
struct LinkArea {
    int x, y, width, height;
    std::string url;
    std::string type;  // 链接类型：footer, cuda, cudnn, tensorrt
};

std::vector<LinkArea> g_linkAreas;

// 窗口配置结构
struct WindowConfig {
    int x = -1;
    int y = -1;
    int width = 450;
    int height = 1000;
    bool autoStart = false;
    bool wasSnapped = false;    // 上次关闭时是否处于吸附状态
    int snapSide = 0;           // 上次吸附的边（1=左，2=右，3=上，4=下）
};

WindowConfig g_windowConfig;

// UI配置 - 硬编码
namespace UIConfig {
    const wchar_t* windowTitle = L"待办清单管理工具";
    const wchar_t* mainTitle = L"智能待办清单";
    const wchar_t* subTitle = L"高效任务管理，让生活更有序";
    const wchar_t* description = L"简洁易用的任务管理工具，帮助您高效组织和完成各项任务";
    const wchar_t* copyright = L"© 2024 待办清单管理工具 - 版权所有";
    const wchar_t* websiteText = L"帮助文档";
    const char* websiteUrl = "https://github.com/todolist";
    const wchar_t* serviceText = L"意见反馈";
    const char* serviceUrl = "mailto:feedback@todolist.com";
}

// 获取任务状态的显示文本
std::string GetTaskStatusText(TaskStatus status) {
    switch (status) {
        case TASK_PENDING: return "待完成";
        case TASK_IN_PROGRESS: return "进行中";
        case TASK_COMPLETED: return "已完成";
        case TASK_CANCELLED: return "已取消";
        default: return "未知";
    }
}

// 获取任务优先级的显示文本
std::string GetTaskPriorityText(TaskPriority priority) {
    switch (priority) {
        case PRIORITY_LOW: return "低";
        case PRIORITY_NORMAL: return "普通";
        case PRIORITY_HIGH: return "高";
        case PRIORITY_URGENT: return "紧急";
        default: return "普通";
    }
}

// 获取任务状态对应的颜色
Color GetTaskStatusColor(TaskStatus status) {
    switch (status) {
        case TASK_PENDING: return Color(156, 163, 175);      // 灰色
        case TASK_IN_PROGRESS: return Color(59, 130, 246);   // 蓝色
        case TASK_COMPLETED: return Color(34, 197, 94);      // 绿色
        case TASK_CANCELLED: return Color(239, 68, 68);      // 红色
        default: return Color(156, 163, 175);
    }
}

// 获取任务优先级对应的颜色
Color GetTaskPriorityColor(TaskPriority priority) {
    switch (priority) {
        case PRIORITY_LOW: return Color(156, 163, 175);      // 灰色
        case PRIORITY_NORMAL: return Color(59, 130, 246);    // 蓝色
        case PRIORITY_HIGH: return Color(245, 158, 11);      // 橙色
        case PRIORITY_URGENT: return Color(239, 68, 68);     // 红色
        default: return Color(59, 130, 246);
    }
}

// 简单的JSON写入函数 - 保存待办清单
void SaveTodoListToJSON(const TodoManager& manager) {
    std::ofstream file(CONFIG_FILE, std::ios::binary);
    if (!file.is_open()) return;

    // 写入UTF-8 BOM
    file.write("\xEF\xBB\xBF", 3);

    file << "{\n";
    file << "  \"nextId\": " << manager.nextId << ",\n";
    file << "  \"tasks\": [\n";

    for (size_t i = 0; i < manager.tasks.size(); ++i) {
        const TaskItem& task = manager.tasks[i];
        file << "    {\n";
        file << "      \"id\": " << task.id << ",\n";
        file << "      \"title\": \"" << EscapeJsonString(task.title) << "\",\n";
        file << "      \"description\": \"" << EscapeJsonString(task.description) << "\",\n";
        file << "      \"status\": " << (int)task.status << ",\n";
        file << "      \"priority\": " << (int)task.priority << ",\n";
        file << "      \"createTime\": \"" << EscapeJsonString(task.createTime) << "\",\n";
        file << "      \"completeTime\": \"" << EscapeJsonString(task.completeTime) << "\"\n";
        file << "    }";
        if (i < manager.tasks.size() - 1) {
            file << ",";
        }
        file << "\n";
    }

    file << "  ]\n";
    file << "}\n";

    file.close();
}

// 简单的JSON读取函数 - 加载待办清单
bool LoadTodoListFromJSON(TodoManager& manager) {
    std::ifstream file(CONFIG_FILE);
    if (!file.is_open()) return false;

    std::string line;
    std::string content;
    while (std::getline(file, line)) {
        content += line;
    }
    file.close();

    // 简单的JSON解析（查找键值对）
    auto findValue = [&content](const std::string& key) -> std::string {
        std::string searchKey = "\"" + key + "\": \"";
        size_t pos = content.find(searchKey);
        if (pos == std::string::npos) return "";

        pos += searchKey.length();
        size_t endPos = content.find("\"", pos);
        if (endPos == std::string::npos) return "";

        return content.substr(pos, endPos - pos);
    };

    auto findIntValue = [&content](const std::string& key) -> int {
        std::string searchKey = "\"" + key + "\": ";
        size_t pos = content.find(searchKey);
        if (pos == std::string::npos) return 0;

        pos += searchKey.length();
        size_t endPos = content.find_first_of(",\n}", pos);
        if (endPos == std::string::npos) return 0;

        std::string valueStr = content.substr(pos, endPos - pos);
        return std::stoi(valueStr);
    };

    try {
        manager.nextId = findIntValue("nextId");
        if (manager.nextId == 0) manager.nextId = 1;

        manager.tasks.clear();

        // 简单解析任务数组（这里简化处理，实际项目建议使用专业JSON库）
        size_t tasksPos = content.find("\"tasks\": [");
        if (tasksPos != std::string::npos) {
            size_t taskStart = tasksPos;
            while ((taskStart = content.find("{", taskStart)) != std::string::npos) {
                size_t taskEnd = content.find("}", taskStart);
                if (taskEnd == std::string::npos) break;

                std::string taskContent = content.substr(taskStart, taskEnd - taskStart + 1);

                TaskItem task;

                // 解析任务字段
                auto findTaskValue = [&taskContent](const std::string& key) -> std::string {
                    std::string searchKey = "\"" + key + "\": \"";
                    size_t pos = taskContent.find(searchKey);
                    if (pos == std::string::npos) return "";
                    pos += searchKey.length();
                    size_t endPos = taskContent.find("\"", pos);
                    if (endPos == std::string::npos) return "";
                    return taskContent.substr(pos, endPos - pos);
                };

                auto findTaskIntValue = [&taskContent](const std::string& key) -> int {
                    std::string searchKey = "\"" + key + "\": ";
                    size_t pos = taskContent.find(searchKey);
                    if (pos == std::string::npos) return 0;
                    pos += searchKey.length();
                    size_t endPos = taskContent.find_first_of(",\n}", pos);
                    if (endPos == std::string::npos) return 0;
                    std::string valueStr = taskContent.substr(pos, endPos - pos);
                    return std::stoi(valueStr);
                };

                task.id = findTaskIntValue("id");
                task.title = UnescapeJsonString(findTaskValue("title"));
                task.description = UnescapeJsonString(findTaskValue("description"));
                task.status = (TaskStatus)findTaskIntValue("status");
                task.priority = (TaskPriority)findTaskIntValue("priority");
                task.createTime = UnescapeJsonString(findTaskValue("createTime"));
                task.completeTime = UnescapeJsonString(findTaskValue("completeTime"));

                manager.tasks.push_back(task);
                taskStart = taskEnd + 1;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

// 清除配置文件
void ClearTodoList() {
    // 直接转换为宽字符
    std::wstring wConfigFile(CONFIG_FILE.begin(), CONFIG_FILE.end());
    DeleteFileW(wConfigFile.c_str());

    // 重置待办清单
    g_todoManager = TodoManager();

    std::lock_guard<std::mutex> lock(g_dataMutex);
    g_statusText = "待办清单已清空";
}

// 已移除UI配置文件相关函数，使用硬编码

// 添加新任务
void AddNewTask() {
    // 获取输入框内容 - 使用Unicode版本
    wchar_t titleBuffer[256];
    wchar_t descBuffer[512];

    GetWindowTextW(g_hEditTask, titleBuffer, sizeof(titleBuffer)/sizeof(wchar_t));
    GetWindowTextW(g_hEditDesc, descBuffer, sizeof(descBuffer)/sizeof(wchar_t));

    // 转换为UTF-8字符串
    std::string title = WStringToString(titleBuffer);
    std::string desc = WStringToString(descBuffer);

    // 检查标题是否为空
    if (title.empty()) {
        MessageBoxW(g_hWnd, L"请输入任务标题！", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 添加任务
    g_todoManager.addTask(title, desc, PRIORITY_NORMAL);

    // 清空输入框
    SetWindowTextW(g_hEditTask, L"");
    SetWindowTextW(g_hEditDesc, L"");

    // 保存到文件
    SaveTodoListToJSON(g_todoManager);

    // 更新状态
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_statusText = "已添加新任务: " + title;
    }

    // 刷新界面
    if (g_hWnd) {
        InvalidateRect(g_hWnd, NULL, TRUE);
    }
}

// 开始编辑任务
void StartEditTask(int taskId) {
    auto it = std::find_if(g_todoManager.tasks.begin(), g_todoManager.tasks.end(),
        [taskId](const TaskItem& task) { return task.id == taskId; });

    if (it != g_todoManager.tasks.end()) {
        g_isEditing = true;
        g_editingTaskId = taskId;

        // 将任务内容填入输入框
        std::wstring wTitle = StringToWString(it->title);
        std::wstring wDesc = StringToWString(it->description);
        SetWindowTextW(g_hEditTask, wTitle.c_str());
        SetWindowTextW(g_hEditDesc, wDesc.c_str());

        // 显示保存和取消按钮，隐藏添加按钮
        ShowWindow(g_hBtnAdd, SW_HIDE);
        ShowWindow(g_hBtnSave, SW_SHOW);
        ShowWindow(g_hBtnCancel, SW_SHOW);

        // 更新状态
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_statusText = "正在编辑任务: " + it->title;
        }

        // 刷新界面
        if (g_hWnd) {
            InvalidateRect(g_hWnd, NULL, TRUE);
        }
    }
}

// 保存编辑的任务
void SaveEditTask() {
    if (!g_isEditing || g_editingTaskId == -1) return;

    auto it = std::find_if(g_todoManager.tasks.begin(), g_todoManager.tasks.end(),
        [](const TaskItem& task) { return task.id == g_editingTaskId; });

    if (it != g_todoManager.tasks.end()) {
        // 获取输入框内容
        wchar_t titleBuffer[256];
        wchar_t descBuffer[512];

        GetWindowTextW(g_hEditTask, titleBuffer, sizeof(titleBuffer)/sizeof(wchar_t));
        GetWindowTextW(g_hEditDesc, descBuffer, sizeof(descBuffer)/sizeof(wchar_t));

        std::string title = WStringToString(titleBuffer);
        std::string desc = WStringToString(descBuffer);

        // 检查标题是否为空
        if (title.empty()) {
            MessageBoxW(g_hWnd, L"请输入任务标题！", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }

        // 更新任务
        it->title = title;
        it->description = desc;

        // 保存到文件
        SaveTodoListToJSON(g_todoManager);

        // 更新状态
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_statusText = "任务已保存: " + title;
        }
    }

    CancelEdit();
}

// 取消编辑
void CancelEdit() {
    g_isEditing = false;
    g_editingTaskId = -1;

    // 清空输入框
    SetWindowTextW(g_hEditTask, L"");
    SetWindowTextW(g_hEditDesc, L"");

    // 显示添加按钮，隐藏保存和取消按钮
    ShowWindow(g_hBtnAdd, SW_SHOW);
    ShowWindow(g_hBtnSave, SW_HIDE);
    ShowWindow(g_hBtnCancel, SW_HIDE);

    // 更新状态
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_statusText = g_isEditing ? "编辑已取消" : "欢迎使用待办清单管理工具";
    }

    // 刷新界面
    if (g_hWnd) {
        InvalidateRect(g_hWnd, NULL, TRUE);
    }
}

// 切换任务状态（勾选框点击）
void ToggleTaskStatus(int taskId) {
    auto it = std::find_if(g_todoManager.tasks.begin(), g_todoManager.tasks.end(),
        [taskId](const TaskItem& task) { return task.id == taskId; });

    if (it != g_todoManager.tasks.end()) {
        // 只在待完成和已完成之间切换
        if (it->status == TASK_COMPLETED) {
            it->status = TASK_PENDING;
            it->completeTime = "";  // 清除完成时间
        } else {
            it->status = TASK_COMPLETED;
            // 设置完成时间
            time_t now = time(0);
            char timeStr[100];
            struct tm timeinfo;
            localtime_s(&timeinfo, &now);
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
            it->completeTime = timeStr;
        }

        // 保存到文件
        SaveTodoListToJSON(g_todoManager);

        // 更新状态
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_statusText = "任务状态已更新: " + it->title;
        }

        // 刷新界面
        if (g_hWnd) {
            InvalidateRect(g_hWnd, NULL, TRUE);
        }
    }
}

// 删除任务
void DeleteTask(int taskId) {
    if (g_todoManager.removeTask(taskId)) {
        // 保存到文件
        SaveTodoListToJSON(g_todoManager);

        // 更新状态
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_statusText = "任务已删除";
        }

        // 刷新界面
        if (g_hWnd) {
            InvalidateRect(g_hWnd, NULL, TRUE);
        }
    }
}

// 获取任务统计信息
struct TaskStats {
    int total;
    int pending;
    int inProgress;
    int completed;
    int cancelled;
};

TaskStats GetTaskStats() {
    TaskStats stats = {0, 0, 0, 0, 0};

    for (const auto& task : g_todoManager.tasks) {
        stats.total++;
        switch (task.status) {
            case TASK_PENDING: stats.pending++; break;
            case TASK_IN_PROGRESS: stats.inProgress++; break;
            case TASK_COMPLETED: stats.completed++; break;
            case TASK_CANCELLED: stats.cancelled++; break;
        }
    }

    return stats;
}

// 更新状态文本
void UpdateStatus(const std::string& status) {
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_statusText = status;
    }

    // 重绘状态文字区域
    if (g_hWnd) {
        RECT statusRect = {60, 110, 800, 130};
        InvalidateRect(g_hWnd, &statusRect, FALSE);
    }
}

// 任务点击区域结构
struct TaskClickArea {
    int taskId;
    int x, y, width, height;
    std::string action;  // "toggle", "delete", "edit"
};

std::vector<TaskClickArea> g_taskClickAreas;

// 绘制任务卡片
void DrawTaskCard(Graphics& graphics, const TaskItem& task, int x, int y, int width, int height) {
    // 卡片阴影
    SolidBrush shadowBrush(Color(15, 0, 0, 0));
    Pen transparentPen(Color::Transparent);
    DrawRoundedRect(graphics, shadowBrush, transparentPen, x + 3, y + 3, width, height, 12);

    // 卡片背景
    SolidBrush cardBrush(Color(255, 255, 255));
    Pen cardPen(Color(240, 242, 247), 2);
    DrawRoundedRect(graphics, cardBrush, cardPen, x, y, width, height, 12);

    // 根据状态选择颜色
    Color statusColor = GetTaskStatusColor(task.status);
    Color priorityColor = GetTaskPriorityColor(task.priority);

    // 左侧状态指示条
    SolidBrush statusBarBrush(statusColor);
    graphics.FillRectangle(&statusBarBrush, x + 1, y + 1, 4, height - 2);

    // 字体设置
    FontFamily fontFamily(L"Microsoft YaHei");

    // 绘制勾选框
    int checkboxSize = 16;  // 减小勾选框
    int checkboxX = x + 10; // 减少左边距
    int checkboxY = y + (height - checkboxSize) / 2;

    // 勾选框背景
    SolidBrush checkboxBrush(task.status == TASK_COMPLETED ? statusColor : Color(255, 255, 255));
    Pen checkboxPen(Color(200, 200, 200), 2);
    graphics.FillRectangle(&checkboxBrush, checkboxX, checkboxY, checkboxSize, checkboxSize);
    graphics.DrawRectangle(&checkboxPen, checkboxX, checkboxY, checkboxSize, checkboxSize);

    // 如果任务完成，绘制勾选标记
    if (task.status == TASK_COMPLETED) {
        Pen checkPen(Color(255, 255, 255), 3);
        graphics.DrawLine(&checkPen, checkboxX + 4, checkboxY + 10, checkboxX + 8, checkboxY + 14);
        graphics.DrawLine(&checkPen, checkboxX + 8, checkboxY + 14, checkboxX + 16, checkboxY + 6);
    }

    // 添加勾选框点击区域
    TaskClickArea checkboxArea;
    checkboxArea.taskId = task.id;
    checkboxArea.x = checkboxX;
    checkboxArea.y = checkboxY;
    checkboxArea.width = checkboxSize;
    checkboxArea.height = checkboxSize;
    checkboxArea.action = "toggle";
    g_taskClickAreas.push_back(checkboxArea);

    // 第一行：任务标题（左侧）+ 日期（右侧）
    Gdiplus::Font titleFont(&fontFamily, 14, FontStyleBold, UnitPixel);  // 增大标题字体
    SolidBrush titleBrush(task.status == TASK_COMPLETED ? Color(150, 150, 150) : Color(31, 41, 55));
    std::wstring wTitle = StringToWString(task.title);

    // 限制标题长度，为日期留出空间
    if (wTitle.length() > 25) {
        wTitle = wTitle.substr(0, 22) + L"...";
    }
    graphics.DrawString(wTitle.c_str(), -1, &titleFont, PointF(x + 35, y + 8), &titleBrush);

    // 日期显示在第一行右侧
    Gdiplus::Font dateFont(&fontFamily, 10, FontStyleRegular, UnitPixel);
    SolidBrush dateBrush(task.status == TASK_COMPLETED ? Color(150, 150, 150) : Color(156, 163, 175));
    std::wstring wCreateTime = StringToWString(task.createTime.substr(0, 10)); // 只显示日期部分

    // 计算日期文字宽度，右对齐
    RectF dateRect;
    graphics.MeasureString(wCreateTime.c_str(), -1, &dateFont, PointF(0, 0), &dateRect);
    float dateX = x + width - dateRect.Width - 15;
    graphics.DrawString(wCreateTime.c_str(), -1, &dateFont, PointF(dateX, y + 10), &dateBrush);

    // 第二行：任务描述 - 最多显示2行
    if (!task.description.empty()) {
        Gdiplus::Font descFont(&fontFamily, 11, FontStyleRegular, UnitPixel);
        SolidBrush descBrush(task.status == TASK_COMPLETED ? Color(150, 150, 150) : Color(107, 114, 128));
        std::wstring wDesc = StringToWString(task.description);

        // 计算可用宽度（去掉左边距和右边距）
        int availableWidth = width - 50;  // 35(左边距) + 15(右边距)
        
        // 测量文本尺寸
        RectF layoutRect(0, 0, (float)availableWidth, 50.0f);  // 限制为2行高度
        RectF boundingBox;
        
        // 如果文本太长，需要截断到最多2行
        std::wstring displayText = wDesc;
        graphics.MeasureString(displayText.c_str(), -1, &descFont, layoutRect, nullptr, &boundingBox);
        
        // 如果超过2行的高度（大约22像素），需要截断
        if (boundingBox.Height > 22.0f) {
            // 逐步减少字符数，直到符合2行要求
            for (size_t len = displayText.length(); len > 0; len--) {
                std::wstring testText = displayText.substr(0, len) + L"...";
                graphics.MeasureString(testText.c_str(), -1, &descFont, layoutRect, nullptr, &boundingBox);
                if (boundingBox.Height <= 22.0f) {
                    displayText = testText;
                    break;
                }
            }
        }
        
        // 绘制描述文本，限制在指定区域内
        RectF drawRect((float)(x + 35), (float)(y + 30), (float)availableWidth, 22.0f);
        graphics.DrawString(displayText.c_str(), -1, &descFont, drawRect, nullptr, &descBrush);
    }





    // 添加任务卡片点击区域（用于编辑）
    TaskClickArea cardArea;
    cardArea.taskId = task.id;
    cardArea.x = x + 35;  // 避开勾选框区域
    cardArea.y = y;
    cardArea.width = width - 50;  // 留出右边距
    cardArea.height = height;
    cardArea.action = "edit";
    g_taskClickAreas.push_back(cardArea);
}





// 绘制圆角矩形
void DrawRoundedRect(Graphics& graphics, Brush& brush, Pen& pen, int x, int y, int width, int height, int radius) {
    GraphicsPath path;
    path.AddArc(x, y, radius * 2, radius * 2, 180, 90);
    path.AddArc(x + width - radius * 2, y, radius * 2, radius * 2, 270, 90);
    path.AddArc(x + width - radius * 2, y + height - radius * 2, radius * 2, radius * 2, 0, 90);
    path.AddArc(x, y + height - radius * 2, radius * 2, radius * 2, 90, 90);
    path.CloseFigure();

    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}

// 绘制渐变背景
void DrawGradientBackground(Graphics& graphics, int width, int height) {
    LinearGradientBrush gradientBrush(
        Point(0, 0),
        Point(0, height),
        Color(245, 247, 250),  // 浅蓝灰色
        Color(255, 255, 255)   // 白色
    );
    graphics.FillRectangle(&gradientBrush, 0, 0, width, height);
}

// 字符串转换函数 - 处理UTF-8编码的字符串
std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return L"";

    // 使用UTF-8编码（因为源文件是UTF-8）
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (size > 0) {
        std::wstring result(size - 1, 0);
        if (MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size) > 0) {
            return result;
        }
    }

    return L"";
}

// 宽字符串转换为UTF-8字符串
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";

    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size > 0) {
        std::string result(size - 1, 0);
        if (WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size, nullptr, nullptr) > 0) {
            return result;
        }
    }

    return "";
}

// 专门处理配置文件中的UTF-8字符串（现在与StringToWString一致）
std::wstring ConfigStringToWString(const std::string& str) {
    return StringToWString(str);  // 统一使用UTF-8处理
}

// JSON字符串转义函数
std::string EscapeJsonString(const std::string& str) {
    std::string result;
    result.reserve(str.size() * 2);  // 预留空间
    
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default: result += c; break;
        }
    }
    return result;
}

// JSON字符串反转义函数
std::string UnescapeJsonString(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            switch (str[i + 1]) {
                case '"': result += '"'; i++; break;
                case '\\': result += '\\'; i++; break;
                case 'n': result += '\n'; i++; break;
                case 'r': result += '\r'; i++; break;
                case 't': result += '\t'; i++; break;
                case 'b': result += '\b'; i++; break;
                case 'f': result += '\f'; i++; break;
                default: result += str[i]; break;
            }
        } else {
            result += str[i];
        }
    }
    return result;
}



// 绘制主界面
void DrawMainInterface(HDC hdc, RECT& clientRect) {
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

    // 绘制渐变背景
    DrawGradientBackground(graphics, clientRect.right, clientRect.bottom);

    // 顶部标题区域背景
    SolidBrush headerBrush(Color(255, 255, 255));
    graphics.FillRectangle(&headerBrush, 0, 0, clientRect.right, 140);

    // 添加底部分割线
    Pen separatorPen(Color(240, 242, 247), 2);
    graphics.DrawLine(&separatorPen, 0, 140, clientRect.right, 140);

    // 软件标题区域
    FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font mainTitleFont(&fontFamily, 26, FontStyleBold, UnitPixel);
    SolidBrush mainTitleBrush(Color(31, 41, 55));

    // 主标题
    graphics.DrawString(UIConfig::mainTitle, -1, &mainTitleFont, PointF(60, 20), &mainTitleBrush);

    // 副标题
    Gdiplus::Font subTitleFont(&fontFamily, 16, FontStyleRegular, UnitPixel);
    SolidBrush subTitleBrush(Color(59, 130, 246));

    graphics.DrawString(UIConfig::subTitle, -1, &subTitleFont, PointF(60, 55), &subTitleBrush);



    // 状态文字和任务统计
    Gdiplus::Font statusFont(&fontFamily, 16, FontStyleRegular, UnitPixel);
    SolidBrush statusBrush(Color(107, 114, 128));

    std::lock_guard<std::mutex> lock(g_dataMutex);

    // 获取任务统计
    TaskStats stats = GetTaskStats();
    std::stringstream statsText;
    statsText << "总任务: " << stats.total << " | 待完成: " << stats.pending
              << " | 进行中: " << stats.inProgress << " | 已完成: " << stats.completed;

    std::wstring wStatsText = StringToWString(statsText.str());
    graphics.DrawString(wStatsText.c_str(), -1, &statusFont, PointF(60, 110), &statusBrush);

    // 清空任务按钮 (右上角) - 现代化设计
    SolidBrush clearBtnBrush(Color(239, 68, 68));  // 红色
    Pen clearBtnPen(Color(220, 60, 60), 2);
    DrawRoundedRect(graphics, clearBtnBrush, clearBtnPen, clientRect.right - 90, 30, 70, 35, 10);

    // 添加按钮阴影效果
    SolidBrush shadowBrush(Color(30, 0, 0, 0));
    Pen transparentPen(Color::Transparent);
    DrawRoundedRect(graphics, shadowBrush, transparentPen, clientRect.right - 88, 32, 70, 35, 10);

    Gdiplus::Font btnFont(&fontFamily, 12, FontStyleBold, UnitPixel);
    SolidBrush btnTextBrush(Color(255, 255, 255));
    graphics.DrawString(L"清空", -1, &btnFont, PointF(clientRect.right - 75, 42), &btnTextBrush);

    // 绘制输入区域标签
    Gdiplus::Font labelFont(&fontFamily, 12, FontStyleBold, UnitPixel);
    SolidBrush labelBrush(Color(31, 41, 55));
    graphics.DrawString(L"任务标题:", -1, &labelFont, PointF(20, 130), &labelBrush);
    graphics.DrawString(L"任务描述:", -1, &labelFont, PointF(20, 165), &labelBrush);

    // 任务列表区域 - 上下两个独立滚动区域，动态调整高度
    int taskStartY = 250;  // 调整位置，去掉描述文字后可以向上移动
    int leftMargin = 20;
    int rightMargin = 20;
    int taskWidth = clientRect.right - leftMargin - rightMargin;
    int taskHeight = 60;
    int taskSpacing = 8;
    int bottomMargin = 60;  // 为底部版权信息留出空间
    int availableHeight = clientRect.bottom - taskStartY - bottomMargin;
    int halfHeight = availableHeight / 2;
    int separatorY = taskStartY + halfHeight;

    // 清空点击区域
    g_taskClickAreas.clear();

    // 分离已完成和未完成的任务
    std::vector<TaskItem> pendingTasks;
    std::vector<TaskItem> completedTasks;

    for (const auto& task : g_todoManager.tasks) {
        if (task.status == TASK_COMPLETED) {
            completedTasks.push_back(task);
        } else {
            pendingTasks.push_back(task);
        }
    }

    // 计算待完成任务区域的滚动 - 动态高度
    int pendingContentHeight = 0;
    if (!pendingTasks.empty()) {
        pendingContentHeight = pendingTasks.size() * (taskHeight + taskSpacing);
    }
    int pendingAreaHeight = halfHeight - 35; // 减去标题高度
    g_maxPendingScrollPos = pendingContentHeight > pendingAreaHeight ? pendingContentHeight - pendingAreaHeight : 0;
    if (g_pendingScrollPos > g_maxPendingScrollPos) g_pendingScrollPos = g_maxPendingScrollPos;
    if (g_pendingScrollPos < 0) g_pendingScrollPos = 0;

    // 计算已完成任务区域的滚动 - 动态高度
    int completedContentHeight = 0;
    if (!completedTasks.empty()) {
        completedContentHeight = completedTasks.size() * (taskHeight + taskSpacing);
    }
    int completedAreaHeight = halfHeight - 35; // 减去标题高度
    g_maxCompletedScrollPos = completedContentHeight > completedAreaHeight ? completedContentHeight - completedAreaHeight : 0;
    if (g_completedScrollPos > g_maxCompletedScrollPos) g_completedScrollPos = g_maxCompletedScrollPos;
    if (g_completedScrollPos < 0) g_completedScrollPos = 0;

    // 绘制待完成任务区域
    Gdiplus::Font sectionFont(&fontFamily, 14, FontStyleBold, UnitPixel);
    SolidBrush sectionBrush(Color(31, 41, 55));
    graphics.DrawString(L"待完成任务", -1, &sectionFont, PointF(leftMargin, taskStartY), &sectionBrush);

    // 设置待完成任务区域的裁剪区域 - 使用动态高度
    int pendingClipY = taskStartY + 25;
    graphics.SetClip(Rect(leftMargin, pendingClipY, taskWidth, pendingAreaHeight));

    int currentY = pendingClipY - g_pendingScrollPos;
    if (pendingTasks.empty()) {
        Gdiplus::Font emptyFont(&fontFamily, 12, FontStyleRegular, UnitPixel);
        SolidBrush emptyBrush(Color(156, 163, 175));
        graphics.DrawString(L"暂无待完成任务", -1, &emptyFont, PointF(leftMargin, currentY + 20), &emptyBrush);
    } else {
        for (const auto& task : pendingTasks) {
            if (currentY + taskHeight > pendingClipY && currentY < separatorY) {
                DrawTaskCard(graphics, task, leftMargin, currentY, taskWidth, taskHeight);
            }
            currentY += taskHeight + taskSpacing;
        }
    }

    // 重置裁剪区域
    graphics.ResetClip();

    // 绘制分割线
    Pen taskSeparatorPen(Color(220, 220, 220), 2);
    graphics.DrawLine(&taskSeparatorPen, leftMargin, separatorY, clientRect.right - rightMargin, separatorY);

    // 绘制已完成任务区域
    SolidBrush completedSectionBrush(Color(107, 114, 128));
    graphics.DrawString(L"已完成任务", -1, &sectionFont, PointF(leftMargin, separatorY + 10), &completedSectionBrush);

    // 设置已完成任务区域的裁剪区域 - 使用动态高度
    int completedClipY = separatorY + 35;
    graphics.SetClip(Rect(leftMargin, completedClipY, taskWidth, completedAreaHeight));

    currentY = completedClipY - g_completedScrollPos;
    if (completedTasks.empty()) {
        Gdiplus::Font emptyFont(&fontFamily, 12, FontStyleRegular, UnitPixel);
        SolidBrush emptyBrush(Color(156, 163, 175));
        graphics.DrawString(L"暂无已完成任务", -1, &emptyFont, PointF(leftMargin, currentY + 20), &emptyBrush);
    } else {
        for (const auto& task : completedTasks) {
            if (currentY + taskHeight > completedClipY && currentY < completedClipY + completedAreaHeight) {
                DrawTaskCard(graphics, task, leftMargin, currentY, taskWidth, taskHeight);
            }
            currentY += taskHeight + taskSpacing;
        }
    }

    // 重置裁剪区域
    graphics.ResetClip();

    // 底部版权信息
    int footerY = clientRect.bottom - 60;

    // 版权文字
    Gdiplus::Font copyrightFont(&fontFamily, 14, FontStyleRegular, UnitPixel);
    SolidBrush copyrightBrush(Color(102, 102, 102));

    graphics.DrawString(UIConfig::copyright, -1, &copyrightFont, PointF(60, footerY), &copyrightBrush);
}

// 边界约束型窗口吸附功能
void CheckWindowSnap(HWND hwnd) {
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    bool shouldSnap = false;
    int newX = windowRect.left;
    int newY = windowRect.top;
    int snapSide = 0;

    // 检查吸附到边缘 - 磁性效果
    // 左边吸附
    if (windowRect.left <= SNAP_DISTANCE && windowRect.left >= -SNAP_DISTANCE) {
        newX = 0;
        snapSide = 1;
        shouldSnap = true;
    }
    // 右边吸附
    else if (windowRect.right >= screenWidth - SNAP_DISTANCE && windowRect.right <= screenWidth + SNAP_DISTANCE) {
        newX = screenWidth - windowWidth;
        snapSide = 2;
        shouldSnap = true;
    }
    
    // 上边吸附
    if (windowRect.top <= SNAP_DISTANCE && windowRect.top >= -SNAP_DISTANCE) {
        newY = 0;
        snapSide = 3;
        shouldSnap = true;
    }
    // 下边吸附
    else if (windowRect.bottom >= screenHeight - SNAP_DISTANCE && windowRect.bottom <= screenHeight + SNAP_DISTANCE) {
        newY = screenHeight - windowHeight;
        snapSide = 4;
        shouldSnap = true;
    }

    if (shouldSnap) {
        // 保存正常位置（吸附前的位置）
        if (!g_isSnapped) {
            g_normalRect = windowRect;
        }

        // 移动到吸附位置
        SetWindowPos(hwnd, NULL, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        g_isSnapped = true;
        g_snapSide = snapSide;

        // 启动隐藏定时器
        if (g_hideTimer) {
            KillTimer(hwnd, g_hideTimer);
        }
        g_hideTimer = SetTimer(hwnd, 1001, HIDE_DELAY, NULL);
    } else {
        g_isSnapped = false;
        g_snapSide = 0;
        if (g_hideTimer) {
            KillTimer(hwnd, g_hideTimer);
            g_hideTimer = 0;
        }
    }
}

// 隐藏窗口到边缘
void HideWindowToEdge(HWND hwnd) {
    if (!g_isSnapped || g_isHidden) return;

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    int newX = windowRect.left;
    int newY = windowRect.top;

    switch (g_snapSide) {
        case 1: // 左边 - 留出HIDE_WIDTH像素可见
            newX = -windowWidth + HIDE_WIDTH;
            break;
        case 2: // 右边 - 留出HIDE_WIDTH像素可见
            newX = screenWidth - HIDE_WIDTH;
            break;
        case 3: // 上边 - 留出HIDE_WIDTH像素可见
            newY = -windowHeight + HIDE_WIDTH;
            break;
        case 4: // 下边 - 留出HIDE_WIDTH像素可见
            newY = screenHeight - HIDE_WIDTH;
            break;
    }

    // 保存隐藏位置
    g_hiddenRect = {newX, newY, newX + windowWidth, newY + windowHeight};

    // 移动到隐藏位置
    SetWindowPos(hwnd, NULL, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    
    // 从任务栏隐藏窗口
    ShowWindow(hwnd, SW_HIDE);
    SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
    ShowWindow(hwnd, SW_SHOW);
    
    g_isHidden = true;
}

// 显示窗口
void ShowWindowFromEdge(HWND hwnd) {
    if (!g_isHidden) return;

    // 恢复任务栏显示
    ShowWindow(hwnd, SW_HIDE);
    SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW);
    ShowWindow(hwnd, SW_SHOW);

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    int newX = windowRect.left;
    int newY = windowRect.top;

    switch (g_snapSide) {
        case 1: // 左边
            newX = 0;
            break;
        case 2: // 右边
            newX = screenWidth - windowWidth;
            break;
        case 3: // 上边
            newY = 0;
            break;
        case 4: // 下边
            newY = screenHeight - windowHeight;
            break;
    }

    // 移动到显示位置，并置顶窗口
    SetWindowPos(hwnd, HWND_TOPMOST, newX, newY, 0, 0, SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    SetForegroundWindow(hwnd);
    g_isHidden = false;
}

// 处理鼠标移动
void HandleMouseMove(HWND hwnd) {
    if (!g_isSnapped) return;

    POINT cursorPos;
    GetCursorPos(&cursorPos);

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    // 检查鼠标是否在窗口区域内或隐藏区域内
    bool mouseInWindow = PtInRect(&windowRect, cursorPos);
    bool mouseInHiddenArea = false;

    if (g_isHidden) {
        // 检查鼠标是否在露出的可见区域
        RECT visibleRect = {0};

        switch (g_snapSide) {
            case 1: // 左边 - 检测露出的右边缘
                visibleRect.left = 0;
                visibleRect.right = HIDE_WIDTH;
                visibleRect.top = g_hiddenRect.top;
                visibleRect.bottom = g_hiddenRect.bottom;
                break;
            case 2: // 右边 - 检测露出的左边缘
                {
                    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                    visibleRect.left = screenWidth - HIDE_WIDTH;
                    visibleRect.right = screenWidth;
                    visibleRect.top = g_hiddenRect.top;
                    visibleRect.bottom = g_hiddenRect.bottom;
                }
                break;
            case 3: // 上边 - 检测露出的下边缘
                visibleRect.left = g_hiddenRect.left;
                visibleRect.right = g_hiddenRect.right;
                visibleRect.top = 0;
                visibleRect.bottom = HIDE_WIDTH;
                break;
            case 4: // 下边 - 检测露出的上边缘
                {
                    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                    visibleRect.left = g_hiddenRect.left;
                    visibleRect.right = g_hiddenRect.right;
                    visibleRect.top = screenHeight - HIDE_WIDTH;
                    visibleRect.bottom = screenHeight;
                }
                break;
        }

        mouseInHiddenArea = PtInRect(&visibleRect, cursorPos);
    }

    if (g_isHidden && mouseInHiddenArea) {
        // 鼠标进入隐藏区域，显示窗口
        ShowWindowFromEdge(hwnd);
        if (g_hideTimer) {
            KillTimer(hwnd, g_hideTimer);
            g_hideTimer = 0;
        }
    } else if (!g_isHidden && !mouseInWindow) {
        // 鼠标离开窗口，启动隐藏定时器
        if (!g_hideTimer) {
            g_hideTimer = SetTimer(hwnd, 1001, HIDE_DELAY, NULL);
        }
    } else if (!g_isHidden && mouseInWindow) {
        // 鼠标在窗口内，取消隐藏定时器
        if (g_hideTimer) {
            KillTimer(hwnd, g_hideTimer);
            g_hideTimer = 0;
        }
    }
}

// 全局鼠标钩子过程
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEMOVE) {
        if (g_hWnd && g_isSnapped) {
            HandleMouseMove(g_hWnd);
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// 自启动功能实现

// 获取启动文件夹路径
std::wstring GetStartupFolderPath() {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, path))) {
        return std::wstring(path);
    }
    return L"";
}

// 创建启动快捷方式
bool CreateStartupShortcut() {
    // 初始化COM
    HRESULT hres = CoInitialize(NULL);
    if (FAILED(hres)) return false;

    IShellLinkW* psl = NULL;
    
    // 创建Shell Link对象
    hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&psl);
    if (SUCCEEDED(hres)) {
        IPersistFile* ppf = NULL;

        // 获取当前程序路径
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);

        // 设置快捷方式目标
        psl->SetPath(exePath);
        psl->SetDescription(L"待办清单管理工具");

        // 获取程序所在目录作为工作目录
        std::wstring workDir = exePath;
        size_t lastSlash = workDir.find_last_of(L"\\");
        if (lastSlash != std::wstring::npos) {
            workDir = workDir.substr(0, lastSlash);
            psl->SetWorkingDirectory(workDir.c_str());
        }

        // 获取IPersistFile接口
        hres = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
        if (SUCCEEDED(hres)) {
            // 构建快捷方式文件路径
            std::wstring startupPath = GetStartupFolderPath();
            if (!startupPath.empty()) {
                std::wstring shortcutPath = startupPath + L"\\待办清单管理工具.lnk";
                
                // 保存快捷方式
                hres = ppf->Save(shortcutPath.c_str(), TRUE);
            }
            ppf->Release();
        }
        psl->Release();
    }
    
    CoUninitialize();
    return SUCCEEDED(hres);
}

// 删除启动快捷方式
bool RemoveStartupShortcut() {
    std::wstring startupPath = GetStartupFolderPath();
    if (startupPath.empty()) return false;

    std::wstring shortcutPath = startupPath + L"\\待办清单管理工具.lnk";
    return DeleteFileW(shortcutPath.c_str()) != 0;
}

// 检查是否已启用自启动
bool IsAutoStartEnabled() {
    std::wstring startupPath = GetStartupFolderPath();
    if (startupPath.empty()) return false;

    std::wstring shortcutPath = startupPath + L"\\待办清单管理工具.lnk";
    
    // 检查文件是否存在
    DWORD fileAttr = GetFileAttributesW(shortcutPath.c_str());
    return (fileAttr != INVALID_FILE_ATTRIBUTES && !(fileAttr & FILE_ATTRIBUTE_DIRECTORY));
}

// 设置自启动开关
void SetAutoStart(bool enable) {
    if (enable) {
        if (CreateStartupShortcut()) {
            g_windowConfig.autoStart = true;
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_statusText = "已启用开机自启动";
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_statusText = "设置开机自启动失败";
            }
        }
    } else {
        if (RemoveStartupShortcut()) {
            g_windowConfig.autoStart = false;
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_statusText = "已禁用开机自启动";
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_statusText = "取消开机自启动失败";
            }
        }
    }
    
    // 保存配置到文件
    SaveWindowConfig();
    
    // 刷新界面
    if (g_hWnd) {
        InvalidateRect(g_hWnd, NULL, TRUE);
    }
}

// 添加托盘图标
void AddTrayIcon(HWND hwnd) {
    if (g_isTrayIconAdded) return;
    
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    
    // 使用系统默认图标或加载自定义图标
    HICON hIcon = (HICON)LoadImageW(NULL, L"icon.ico", IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    if (!hIcon) {
        hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    g_trayIcon.hIcon = hIcon;
    
    // 设置提示文字
    lstrcpyW(g_trayIcon.szTip, L"待办清单管理工具");
    
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    g_isTrayIconAdded = true;
}

// 移除托盘图标
void RemoveTrayIcon() {
    if (!g_isTrayIconAdded) return;
    
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_isTrayIconAdded = false;
}

// 显示托盘菜单
void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    
    HMENU hMenu = CreatePopupMenu();
    if (hMenu) {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"显示窗口");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
        
        // 确保菜单在前台显示
        SetForegroundWindow(hwnd);
        
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                      pt.x, pt.y, 0, hwnd, NULL);
        
        DestroyMenu(hMenu);
    }
}

// 保存窗口配置
void SaveWindowConfig() {
    std::ofstream file(WINDOW_CONFIG_FILE, std::ios::binary);
    if (!file.is_open()) return;

    // 写入UTF-8 BOM
    file.write("\xEF\xBB\xBF", 3);
    
    // 获取当前窗口位置和大小
    if (g_hWnd) {
        if (!g_isSnapped && !g_isHidden) {
            // 如果没有吸附也没有隐藏，保存当前位置
            RECT rect;
            GetWindowRect(g_hWnd, &rect);
            g_windowConfig.x = rect.left;
            g_windowConfig.y = rect.top;
            g_windowConfig.width = rect.right - rect.left;
            g_windowConfig.height = rect.bottom - rect.top;
        } else if (g_isSnapped) {
            // 如果已吸附，保存吸附前的正常位置
            g_windowConfig.x = g_normalRect.left;
            g_windowConfig.y = g_normalRect.top;
            g_windowConfig.width = g_normalRect.right - g_normalRect.left;
            g_windowConfig.height = g_normalRect.bottom - g_normalRect.top;
        }
    }

    // 保存当前的吸附状态
    g_windowConfig.wasSnapped = g_isSnapped;
    g_windowConfig.snapSide = g_snapSide;

    file << "{\n";
    file << "  \"x\": " << g_windowConfig.x << ",\n";
    file << "  \"y\": " << g_windowConfig.y << ",\n";
    file << "  \"width\": " << g_windowConfig.width << ",\n";
    file << "  \"height\": " << g_windowConfig.height << ",\n";
    file << "  \"autoStart\": " << (g_windowConfig.autoStart ? "true" : "false") << ",\n";
    file << "  \"wasSnapped\": " << (g_windowConfig.wasSnapped ? "true" : "false") << ",\n";
    file << "  \"snapSide\": " << g_windowConfig.snapSide << "\n";
    file << "}\n";

    file.close();
}

// 加载窗口配置
bool LoadWindowConfig() {
    std::ifstream file(WINDOW_CONFIG_FILE);
    if (!file.is_open()) return false;

    std::string line;
    std::string content;
    while (std::getline(file, line)) {
        content += line;
    }
    file.close();

    // 简单的JSON解析
    auto findIntValue = [&content](const std::string& key) -> int {
        std::string searchKey = "\"" + key + "\": ";
        size_t pos = content.find(searchKey);
        if (pos == std::string::npos) return -1;

        pos += searchKey.length();
        size_t endPos = content.find_first_of(",\n}", pos);
        if (endPos == std::string::npos) return -1;

        std::string valueStr = content.substr(pos, endPos - pos);
        try {
            return std::stoi(valueStr);
        } catch (...) {
            return -1;
        }
    };

    auto findBoolValue = [&content](const std::string& key) -> bool {
        std::string searchKey = "\"" + key + "\": ";
        size_t pos = content.find(searchKey);
        if (pos == std::string::npos) return false;

        pos += searchKey.length();
        size_t endPos = content.find_first_of(",\n}", pos);
        if (endPos == std::string::npos) return false;

        std::string valueStr = content.substr(pos, endPos - pos);
        return valueStr == "true";
    };

    try {
        int x = findIntValue("x");
        int y = findIntValue("y");
        int width = findIntValue("width");
        int height = findIntValue("height");
        
        if (x != -1) g_windowConfig.x = x;
        if (y != -1) g_windowConfig.y = y;
        if (width != -1) g_windowConfig.width = width;
        if (height != -1) g_windowConfig.height = height;
        
        g_windowConfig.autoStart = findBoolValue("autoStart");
        g_windowConfig.wasSnapped = findBoolValue("wasSnapped");
        
        int snapSide = findIntValue("snapSide");
        if (snapSide != -1) g_windowConfig.snapSide = snapSide;

        return true;
    } catch (...) {
        return false;
    }
}

// 窗口过程
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        {
            // 初始化GDI+
            GdiplusStartupInput gdiplusStartupInput;
            ULONG_PTR gdiplusToken;
            GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
            
            // 添加托盘图标
            AddTrayIcon(hwnd);

            // 获取客户区大小用于响应式布局
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int clientWidth = clientRect.right;

            // 创建输入框和按钮控件 - 响应式布局
            int margin = 20;
            int buttonWidth = 80;
            int inputWidth = clientWidth - margin * 3 - buttonWidth;

            // 任务标题输入框
            g_hEditTask = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                margin, 150, inputWidth, 30, hwnd, (HMENU)ID_EDIT_TASK,
                GetModuleHandle(NULL), NULL);

            // 任务描述输入框 - 3行高度，支持多行
            g_hEditDesc = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                margin, 185, inputWidth, 60, hwnd, (HMENU)ID_EDIT_DESC,
                GetModuleHandle(NULL), NULL);

            // 添加按钮 - 使用现代样式
            g_hBtnAdd = CreateWindowW(L"BUTTON", L"➕ 添加任务",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                margin + inputWidth + margin, 150, buttonWidth, 30, hwnd, (HMENU)ID_BUTTON_ADD,
                GetModuleHandle(NULL), NULL);

            // 保存按钮（初始隐藏）- 使用现代样式
            g_hBtnSave = CreateWindowW(L"BUTTON", L"✓ 保存",
                WS_CHILD | BS_PUSHBUTTON | BS_FLAT,
                margin + inputWidth + margin, 150, buttonWidth, 30, hwnd, (HMENU)ID_BUTTON_SAVE,
                GetModuleHandle(NULL), NULL);

            // 取消按钮（初始隐藏）- 使用现代样式
            g_hBtnCancel = CreateWindowW(L"BUTTON", L"✕ 取消",
                WS_CHILD | BS_PUSHBUTTON | BS_FLAT,
                margin + inputWidth + margin, 185, buttonWidth, 30, hwnd, (HMENU)ID_BUTTON_CANCEL,
                GetModuleHandle(NULL), NULL);

            // 设置输入框提示文字 - 使用Unicode版本
            SendMessageW(g_hEditTask, EM_SETCUEBANNER, TRUE, (LPARAM)L"请输入任务标题...");
            SendMessageW(g_hEditDesc, EM_SETCUEBANNER, TRUE, (LPARAM)L"请输入任务描述...");

            // 创建现代化字体用于按钮
            HFONT hButtonFont = CreateFontW(
                16,                        // 字体高度
                0,                         // 字体宽度
                0,                         // 角度
                0,                         // 基线角度
                FW_NORMAL,                 // 字体粗细
                FALSE,                     // 斜体
                FALSE,                     // 下划线
                FALSE,                     // 删除线
                DEFAULT_CHARSET,           // 字符集
                OUT_DEFAULT_PRECIS,        // 输出精度
                CLIP_DEFAULT_PRECIS,       // 裁剪精度
                CLEARTYPE_QUALITY,         // 输出质量
                DEFAULT_PITCH | FF_SWISS,  // 字体族
                L"Microsoft YaHei"         // 字体名称
            );

            // 为按钮设置字体
            if (hButtonFont) {
                SendMessage(g_hBtnAdd, WM_SETFONT, (WPARAM)hButtonFont, TRUE);
                SendMessage(g_hBtnSave, WM_SETFONT, (WPARAM)hButtonFont, TRUE);
                SendMessage(g_hBtnCancel, WM_SETFONT, (WPARAM)hButtonFont, TRUE);
            }

            // 创建开机自启动复选框 - 放在清空按钮下面
            g_hCheckAutoStart = CreateWindowW(L"BUTTON", L"开机自启动",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                clientWidth - 90, 70, 80, 20, hwnd, (HMENU)ID_CHECK_AUTOSTART,
                GetModuleHandle(NULL), NULL);

            // 为复选框设置字体
            if (hButtonFont) {
                HFONT hCheckboxFont = CreateFontW(
                    12,                        // 稍小的字体
                    0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                    L"Microsoft YaHei"
                );
                if (hCheckboxFont) {
                    SendMessage(g_hCheckAutoStart, WM_SETFONT, (WPARAM)hCheckboxFont, TRUE);
                }
            }

            // 安装全局鼠标钩子
            g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);



            // 尝试加载待办清单配置文件
            if (LoadTodoListFromJSON(g_todoManager)) {
                // 如果成功加载配置，更新状态文字
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_statusText = "已加载待办清单数据";
                InvalidateRect(hwnd, NULL, TRUE);
            }

            // 加载窗口配置
            LoadWindowConfig();
            
            // 设置复选框状态 - 检查实际的自启动状态
            bool autoStartEnabled = IsAutoStartEnabled();
            g_windowConfig.autoStart = autoStartEnabled;
            SendMessage(g_hCheckAutoStart, BM_SETCHECK, autoStartEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            
            // 延迟检查窗口吸附状态，确保窗口已完全初始化
            SetTimer(hwnd, 3001, 500, NULL);
        }
        break;

    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case ID_BUTTON_ADD:
                    AddNewTask();
                    break;
                case ID_BUTTON_SAVE:
                    SaveEditTask();
                    break;
                case ID_BUTTON_CANCEL:
                    CancelEdit();
                    break;
                case ID_CHECK_AUTOSTART:
                    {
                        // 处理复选框点击
                        LRESULT checkState = SendMessage(g_hCheckAutoStart, BM_GETCHECK, 0, 0);
                        bool isChecked = (checkState == BST_CHECKED);
                        SetAutoStart(isChecked);
                    }
                    break;
                case ID_TRAY_SHOW:
                    // 显示窗口
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    if (g_isHidden) {
                        ShowWindowFromEdge(hwnd);
                    }
                    break;
                case ID_TRAY_EXIT:
                    // 退出程序
                    DestroyWindow(hwnd);
                    break;
                default:
                    return DefWindowProc(hwnd, uMsg, wParam, lParam);
            }
        }
        break;
    
    case WM_TRAYICON:
        {
            switch (lParam) {
                case WM_LBUTTONUP:
                    // 左键点击托盘图标，显示/隐藏窗口
                    if (IsWindowVisible(hwnd)) {
                        ShowWindow(hwnd, SW_HIDE);
                    } else {
                        ShowWindow(hwnd, SW_RESTORE);
                        SetForegroundWindow(hwnd);
                        if (g_isHidden) {
                            ShowWindowFromEdge(hwnd);
                        }
                    }
                    break;
                case WM_RBUTTONUP:
                    // 右键点击托盘图标，显示菜单
                    ShowTrayMenu(hwnd);
                    break;
            }
        }
        break;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // 双缓冲绘制
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            DrawMainInterface(memDC, clientRect);

            BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
        }
        break;

    case WM_LBUTTONDOWN:
        {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            // 检查是否点击了清空任务按钮
            if (x >= clientRect.right - 90 && x <= clientRect.right - 20 &&
                y >= 30 && y <= 65) {
                // 显示确认对话框
                int result = MessageBoxW(hwnd,
                    L"确定要清空所有任务吗？此操作无法撤销。",
                    L"确认清空",
                    MB_YESNO | MB_ICONQUESTION);

                if (result == IDYES) {
                    ClearTodoList();
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }

            // 检查是否点击了任务相关区域
            for (const auto& clickArea : g_taskClickAreas) {
                if (x >= clickArea.x && x <= clickArea.x + clickArea.width &&
                    y >= clickArea.y && y <= clickArea.y + clickArea.height) {
                    if (clickArea.action == "toggle") {
                        ToggleTaskStatus(clickArea.taskId);
                    } else if (clickArea.action == "edit") {
                        StartEditTask(clickArea.taskId);
                    }
                    break;
                }
            }

        }
        break;

    case WM_ERASEBKGND:
        // 阻止默认的背景擦除，防止闪烁
        return 1;

    case WM_MOUSEMOVE:
        {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            // 处理窗口吸附的鼠标移动
            HandleMouseMove(hwnd);

            // 检查鼠标是否在按钮区域或任务卡片区域
            bool overClickable = false;

            // 检查清空任务按钮
            if (x >= clientRect.right - 90 && x <= clientRect.right - 20 &&
                y >= 30 && y <= 65) {
                overClickable = true;
            }

            // 检查任务卡片区域
            for (const auto& clickArea : g_taskClickAreas) {
                if (x >= clickArea.x && x <= clickArea.x + clickArea.width &&
                    y >= clickArea.y && y <= clickArea.y + clickArea.height) {
                    overClickable = true;
                    break;
                }
            }

            // 设置光标
            if (overClickable) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
            } else {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            }
        }
        break;

    case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int scrollAmount = 3; // 每次滚动的行数
            int mouseX = LOWORD(lParam);
            int mouseY = HIWORD(lParam);

            // 将屏幕坐标转换为客户区坐标
            POINT pt = {mouseX, mouseY};
            ScreenToClient(hwnd, &pt);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            int taskStartY = 250;
            int bottomMargin = 80;  // 为底部版权信息留出空间
            int availableHeight = clientRect.bottom - taskStartY - bottomMargin;
            int halfHeight = availableHeight / 2;
            int separatorY = taskStartY + halfHeight;

            // 根据鼠标位置决定滚动哪个区域
            if (pt.y >= taskStartY && pt.y < separatorY) {
                // 在待完成任务区域
                if (delta > 0) {
                    g_pendingScrollPos -= scrollAmount * 20;
                } else {
                    g_pendingScrollPos += scrollAmount * 20;
                }

                if (g_pendingScrollPos < 0) g_pendingScrollPos = 0;
                if (g_pendingScrollPos > g_maxPendingScrollPos) g_pendingScrollPos = g_maxPendingScrollPos;
            } else if (pt.y >= separatorY && pt.y < clientRect.bottom) {
                // 在已完成任务区域
                if (delta > 0) {
                    g_completedScrollPos -= scrollAmount * 20;
                } else {
                    g_completedScrollPos += scrollAmount * 20;
                }

                if (g_completedScrollPos < 0) g_completedScrollPos = 0;
                if (g_completedScrollPos > g_maxCompletedScrollPos) g_completedScrollPos = g_maxCompletedScrollPos;
            }

            // 重绘窗口
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;

    case WM_SIZE:
        {
            // 窗口大小改变时重新调整控件位置和大小
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int clientWidth = clientRect.right;

            int margin = 20;
            int buttonWidth = 80;
            int inputWidth = clientWidth - margin * 3 - buttonWidth;

            // 调整输入框大小
            if (g_hEditTask) {
                SetWindowPos(g_hEditTask, NULL, margin, 150, inputWidth, 30, SWP_NOZORDER);
            }
            if (g_hEditDesc) {
                SetWindowPos(g_hEditDesc, NULL, margin, 185, inputWidth, 60, SWP_NOZORDER);
            }

            // 调整按钮位置
            int buttonX = margin + inputWidth + margin;
            if (g_hBtnAdd) {
                SetWindowPos(g_hBtnAdd, NULL, buttonX, 150, buttonWidth, 30, SWP_NOZORDER);
            }
            if (g_hBtnSave) {
                SetWindowPos(g_hBtnSave, NULL, buttonX, 150, buttonWidth, 30, SWP_NOZORDER);
            }
            if (g_hBtnCancel) {
                SetWindowPos(g_hBtnCancel, NULL, buttonX, 185, buttonWidth, 30, SWP_NOZORDER);
            }

            // 调整复选框位置 - 右上角清空按钮下面
            if (g_hCheckAutoStart) {
                SetWindowPos(g_hCheckAutoStart, NULL, clientRect.right - 90, 70, 80, 20, SWP_NOZORDER);
            }

            // 重新计算滚动范围并重绘
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;

    case WM_MOVING:
        {
            // 窗口移动时进行边界约束
            RECT* pRect = (RECT*)lParam;
            if (pRect) {
                int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                int windowWidth = pRect->right - pRect->left;
                int windowHeight = pRect->bottom - pRect->top;

                // 边界约束 - 不允许窗口完全拖出屏幕（保留一定容忍度）
                if (pRect->left < -SNAP_DISTANCE) {
                    pRect->left = -SNAP_DISTANCE;
                    pRect->right = pRect->left + windowWidth;
                }
                if (pRect->right > screenWidth + SNAP_DISTANCE) {
                    pRect->right = screenWidth + SNAP_DISTANCE;
                    pRect->left = pRect->right - windowWidth;
                }
                if (pRect->top < -SNAP_DISTANCE) {
                    pRect->top = -SNAP_DISTANCE;
                    pRect->bottom = pRect->top + windowHeight;
                }
                if (pRect->bottom > screenHeight + SNAP_DISTANCE) {
                    pRect->bottom = screenHeight + SNAP_DISTANCE;
                    pRect->top = pRect->bottom - windowHeight;
                }
            }
        }
        break;

    case WM_EXITSIZEMOVE:
        {
            // 窗口移动结束时检查吸附
            CheckWindowSnap(hwnd);
            // 窗口移动或调整大小结束时保存配置
            SetTimer(hwnd, 2001, 100, NULL);
        }
        break;

    case WM_TIMER:
        {
            if (wParam == 1001) {
                // 隐藏定时器触发
                HideWindowToEdge(hwnd);
                KillTimer(hwnd, g_hideTimer);
                g_hideTimer = 0;
            } else if (wParam == 2001) {
                // 保存窗口配置的延迟定时器
                SaveWindowConfig();
                KillTimer(hwnd, 2001);
            } else if (wParam == 3001) {
                // 启动时检查窗口是否应该吸附
                if (g_windowConfig.wasSnapped) {
                    // 如果上次关闭时处于吸附状态，直接设置吸附状态
                    g_isSnapped = true;
                    g_snapSide = g_windowConfig.snapSide;
                    
                    // 保存正常位置（用于位置记忆）
                    g_normalRect.left = g_windowConfig.x;
                    g_normalRect.top = g_windowConfig.y;
                    g_normalRect.right = g_windowConfig.x + g_windowConfig.width;
                    g_normalRect.bottom = g_windowConfig.y + g_windowConfig.height;
                    
                    // 启动隐藏定时器
                    if (g_hideTimer) {
                        KillTimer(hwnd, g_hideTimer);
                    }
                    g_hideTimer = SetTimer(hwnd, 1001, HIDE_DELAY, NULL);
                } else {
                    // 否则检查是否需要吸附
                    CheckWindowSnap(hwnd);
                }
                KillTimer(hwnd, 3001);
            }
            // 可以在这里添加其他定期刷新任务状态的逻辑
        }
        break;

    case WM_DESTROY:
        // 保存窗口配置
        SaveWindowConfig();
        // 移除托盘图标
        RemoveTrayIcon();
        // 移除鼠标钩子
        if (g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// 主函数 - 使用Unicode版本的WinMain
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // 设置区域为中文
    setlocale(LC_ALL, "zh_CN.UTF-8");

    // 设置DPI感知
    SetProcessDPIAware();

    // 初始化GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // 不再加载UI配置文件，使用硬编码

    // 尝试加载待办清单配置
    LoadTodoListFromJSON(g_todoManager);

    // 加载窗口配置
    LoadWindowConfig();

    // 尝试加载自定义图标 - 使用Unicode版本
    HICON hCustomIcon = (HICON)LoadImageW(NULL, L"icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    HICON hIcon = hCustomIcon ? hCustomIcon : LoadIcon(NULL, IDI_APPLICATION);

    // 注册窗口类 - 完全使用Unicode版本
    const wchar_t CLASS_NAME[] = L"TodoListWindow";

    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;  // 使用自己的DC，减少闪烁
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = NULL;  // 不使用默认背景，我们自己绘制
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIcon;

    RegisterClassW(&wc);

    // 使用配置中的窗口位置和大小，或使用默认值
    int windowWidth = g_windowConfig.width;
    int windowHeight = g_windowConfig.height;
    int posX = g_windowConfig.x;
    int posY = g_windowConfig.y;
    
    // 如果配置中没有有效的位置信息，使用屏幕居中
    if (posX == -1 || posY == -1) {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        posX = (screenWidth - windowWidth) / 2;
        posY = (screenHeight - windowHeight) / 2;
    }
    
    // 如果上次关闭时处于吸附状态，调整到正确的吸附位置
    if (g_windowConfig.wasSnapped) {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        
        switch (g_windowConfig.snapSide) {
            case 1: // 左边
                posX = 0;
                break;
            case 2: // 右边
                posX = screenWidth - windowWidth;
                break;
            case 3: // 上边
                posY = 0;
                break;
            case 4: // 下边
                posY = screenHeight - windowHeight;
                break;
        }
    }

    // 创建窗口 - 使用WS_EX_TOOLWINDOW使窗口不在任务栏显示
    g_hWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,  // 不在任务栏显示
        CLASS_NAME,
        UIConfig::windowTitle,  // 使用硬编码的标题
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,  // 可拉伸窗口，但去掉最大化按钮
        posX, posY, windowWidth, windowHeight,  // 使用计算出的居中位置
        NULL, NULL, hInstance, NULL
    );

    if (g_hWnd == NULL) {
        return 0;
    }

    // 为窗口设置图标（大图标和小图标）
    if (hCustomIcon) {
        SendMessage(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hCustomIcon);
        SendMessage(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hCustomIcon);
    }

    // 设置定时器用于更新界面
    SetTimer(g_hWnd, 1, 100, NULL);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 消息循环
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 清理资源
    if (hCustomIcon) {
        DestroyIcon(hCustomIcon);
    }

    // 清理GDI+
    GdiplusShutdown(gdiplusToken);

    return 0;
}
