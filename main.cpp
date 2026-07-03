// QQ 连连看（角色版）自动消除工具
//
// 原理：通过 ReadProcessMemory 读取游戏进程内存中的棋盘，按连连看规则
//       （连接路径不超过两个拐点，且允许绕棋盘外框一圈）计算可连接的同值
//       方块对，再用 SetCursorPos + mouse_event 模拟点击自动消除。
//
// 说明：本工具仅支持 Windows；需以管理员身份运行；源文件为 UTF-8 编码，
//       MSVC 需带 /utf-8 编译选项（本仓库的 .vcxproj 已配置）。

#ifndef _WIN32
#error "本工具依赖 ReadProcessMemory / mouse_event / FindWindowW，仅支持 Windows 平台"
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <utility>
#include <vector>

// 棋盘坐标点：x = 列，y = 行
struct Point {
    int x = 0;
    int y = 0;
    Point(int _x = 0, int _y = 0) : x(_x), y(_y) {}
};

// 一个方块：值 + 棋盘坐标（value == 0 表示空格）
struct Block {
    unsigned char value = 0;
    Point point;

    Block() = default;
    Block(int x, int y, unsigned char v = 0) : value(v), point(x, y) {}
};

class Link {
public:
    Link() = default;
    ~Link() { closeProcess(); }

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    void run() {
        std::cout << "F1 开始\nF2 停止" << std::endl;
        while (true) {
            handleKeyInput();
            if (!isRunning_) {
                Sleep(100);
                continue;
            }
            if (!initializeGame()) {
                Sleep(1000);
                continue;
            }
            runGameLoop();
        }
    }

private:
    // ---- 棋盘几何 ----
    static constexpr int ROWS = 11;  // 行数
    static constexpr int COLS = 19;  // 列数

    // ---- 目标进程内存区域（每格 1 字节，0 表示空）----
    static constexpr uintptr_t START_ADDRESS = 0x199F5C + 4;
    static constexpr uintptr_t END_ADDRESS = 0x19A02C + 4;
    static constexpr size_t REGION_SIZE = END_ADDRESS - START_ADDRESS + 1;
    static_assert(REGION_SIZE == static_cast<size_t>(ROWS) * COLS,
                  "内存区域大小必须等于 ROWS*COLS");

    // ---- 点击标定参数（相对窗口左上角的像素偏移，随游戏窗口样式而定）----
    static constexpr int CLICK_ORIGIN_X = 25;
    static constexpr int CLICK_ORIGIN_Y = 195;
    static constexpr int CELL_W = 31;
    static constexpr int CELL_H = 35;
    static constexpr DWORD CLICK_GAP_MS = 8;  // 每次点击后的短暂延时，避免过快漏点

    // ---- 循环节奏 ----
    static constexpr DWORD LOOP_DELAY_MS = 50;   // 每轮扫描间隔
    static constexpr DWORD EMPTY_WAIT_MS = 300;  // 棋盘为空（过关/切换）时的等待
    static constexpr int NO_MOVE_LIMIT = 3;      // 连续无可消除对达到此值判为死局

    // 带一圈空白外框的占用栅格：真实格 (y,x) 存于 grid_[y+1][x+1]，
    // 外框（下标 0 与 ROWS+1 / COLS+1）恒为空，用于支持“绕外框一圈”的连接。
    std::array<std::array<unsigned char, COLS + 2>, ROWS + 2> grid_{};

    // 按值分组的方块坐标，供匹配算法使用
    std::unordered_map<unsigned char, std::vector<Point>> valueGroups_;
    std::vector<std::pair<Block, Block>> pairs_;

    HANDLE processHandle_ = nullptr;
    HWND windowHandle_ = nullptr;
    bool isRunning_ = false;
    int noMoveCount_ = 0;

    // ---------- 主循环控制 ----------
    void handleKeyInput() {
        if (isKeyPressed(VK_F1) && !isRunning_) {
            isRunning_ = true;
            noMoveCount_ = 0;
            std::cout << "程序已启动，按 F2 暂停" << std::endl;
            Sleep(200);
        } else if (isKeyPressed(VK_F2) && isRunning_) {
            isRunning_ = false;
            std::cout << "程序已暂停，按 F1 继续" << std::endl;
            Sleep(200);
        }
    }

    bool initializeGame() {
        if (!readWindow()) {
            std::cout << "未找到目标进程" << std::endl;
            return false;
        }
        return true;
    }

    void runGameLoop() {
        while (isRunning_) {
            if (isKeyPressed(VK_F2)) {
                isRunning_ = false;
                std::cout << "程序已暂停，按 F1 继续" << std::endl;
                Sleep(200);
                return;
            }

            if (!readBoard()) {
                // 读取失败：游戏多半已关闭/句柄失效，释放并回到外层重新连接
                std::cout << "读取内存失败，正在重新连接……" << std::endl;
                closeProcess();
                windowHandle_ = nullptr;
                Sleep(500);
                return;
            }

            if (valueGroups_.empty()) {
                // 棋盘为空：过关或界面切换，等待而非拆除循环（避免空转 / 句柄泄漏）
                Sleep(EMPTY_WAIT_MS);
                continue;
            }

            findMatchingPairs();

            if (pairs_.empty()) {
                // 有方块却找不到任何可连接对：可能是死局（需洗牌）。
                // 多扫描几轮以排除瞬时状态，仍无解则暂停并提示。
                if (++noMoveCount_ >= NO_MOVE_LIMIT) {
                    std::cout << "未找到可消除的方块对（可能需要洗牌），已暂停。按 F1 继续" << std::endl;
                    isRunning_ = false;
                    return;
                }
                Sleep(EMPTY_WAIT_MS);
                continue;
            }

            noMoveCount_ = 0;
            eliminatePairs();
            Sleep(LOOP_DELAY_MS);
        }
    }

    // ---------- 进程 / 窗口 ----------
    void closeProcess() {
        if (processHandle_) {
            CloseHandle(processHandle_);
            processHandle_ = nullptr;
        }
    }

    bool readWindow() {
        windowHandle_ = FindWindowW(nullptr, L"QQ游戏 - 连连看角色版");
        if (!windowHandle_) {
            std::cout << "未找到目标窗口" << std::endl;
            return false;
        }

        DWORD processId = 0;
        if (GetWindowThreadProcessId(windowHandle_, &processId) == 0 || processId == 0) {
            std::cout << "无法获取窗口进程 ID" << std::endl;
            return false;
        }

        closeProcess();  // 关闭上一轮的句柄，避免泄漏
        processHandle_ = OpenProcess(PROCESS_VM_READ, FALSE, processId);
        if (!processHandle_) {
            DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                std::cout << "打开进程被拒绝，请以管理员身份运行" << std::endl;
            } else {
                std::cout << "打开进程失败，错误码 " << err << std::endl;
            }
            return false;
        }
        return true;
    }

    // ---------- 读取棋盘 ----------
    bool readBoard() {
        if (!processHandle_) return false;

        std::array<unsigned char, REGION_SIZE> buffer{};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(processHandle_,
                               reinterpret_cast<LPCVOID>(START_ADDRESS),
                               buffer.data(), REGION_SIZE, &bytesRead) ||
            bytesRead != REGION_SIZE) {
            return false;
        }

        // 每轮都从内存重建栅格与分组（写入 0 以清除已消除的格子）
        for (auto& row : grid_) row.fill(0);
        valueGroups_.clear();

        for (int y = 0; y < ROWS; ++y) {
            for (int x = 0; x < COLS; ++x) {
                unsigned char value = buffer[static_cast<size_t>(y) * COLS + x];
                setCell(y, x, value);
                if (value != 0) {
                    valueGroups_[value].emplace_back(x, y);
                }
            }
        }
        return true;
    }

    // ---------- 占用栅格访问（含外框）----------
    void setCell(int y, int x, unsigned char v) { grid_[y + 1][x + 1] = v; }

    // 允许查询 [-1, ROWS] × [-1, COLS]（含外框一圈）；范围外视为非空
    bool isEmpty(int y, int x) const {
        if (y < -1 || y > ROWS || x < -1 || x > COLS) return false;
        return grid_[y + 1][x + 1] == 0;
    }

    // ---------- 匹配算法 ----------
    void findMatchingPairs() {
        pairs_.clear();
        for (const auto& [value, points] : valueGroups_) {
            matchGroup(value, points);
        }
    }

    // 对同值方块做贪心匹配：优先拐点少、曼哈顿距离短的连接
    void matchGroup(unsigned char value, const std::vector<Point>& points) {
        std::vector<bool> matched(points.size(), false);

        for (size_t i = 0; i < points.size(); ++i) {
            if (matched[i]) continue;

            int bestIndex = -1;
            int bestTurns = 3;  // 合法连接最多 2 拐点，3 作哨兵
            int bestDistance = INT_MAX;

            for (size_t j = i + 1; j < points.size(); ++j) {
                if (matched[j]) continue;

                int turns = 0;
                if (canConnect(points[i], points[j], turns)) {
                    int distance = manhattan(points[i], points[j]);
                    if (turns < bestTurns ||
                        (turns == bestTurns && distance < bestDistance)) {
                        bestTurns = turns;
                        bestDistance = distance;
                        bestIndex = static_cast<int>(j);
                    }
                }
            }

            if (bestIndex != -1) {
                Block a(points[i].x, points[i].y, value);
                Block b(points[bestIndex].x, points[bestIndex].y, value);
                pairs_.emplace_back(a, b);
                matched[i] = true;
                matched[static_cast<size_t>(bestIndex)] = true;
            }
        }
    }

    static int manhattan(const Point& p1, const Point& p2) {
        return std::abs(p2.x - p1.x) + std::abs(p2.y - p1.y);
    }

    // ---------- 连接判定 ----------
    bool canConnect(const Point& p1, const Point& p2, int& turns) const {
        // 同行/同列且直线畅通：0 拐点。被挡时继续尝试一/两拐点绕行。
        if ((p1.x == p2.x || p1.y == p2.y) && checkStraightLine(p1, p2)) {
            turns = 0;
            return true;
        }
        if (checkOneTurn(p1, p2)) {
            turns = 1;
            return true;
        }
        if (checkTwoTurns(p1, p2)) {
            turns = 2;
            return true;
        }
        return false;
    }

    // 两点必须共行或共列；检查其间的所有格是否全空
    bool checkStraightLine(const Point& p1, const Point& p2) const {
        if (p1.y == p2.y) {
            int lo = std::min(p1.x, p2.x), hi = std::max(p1.x, p2.x);
            for (int x = lo + 1; x < hi; ++x) {
                if (!isEmpty(p1.y, x)) return false;
            }
            return true;
        }
        if (p1.x == p2.x) {
            int lo = std::min(p1.y, p2.y), hi = std::max(p1.y, p2.y);
            for (int y = lo + 1; y < hi; ++y) {
                if (!isEmpty(y, p1.x)) return false;
            }
            return true;
        }
        return false;
    }

    // 一个拐点：拐角落在两点行列交点（恒在棋盘内）
    bool checkOneTurn(const Point& p1, const Point& p2) const {
        Point corner1(p1.x, p2.y);
        if (isEmpty(corner1.y, corner1.x) &&
            checkStraightLine(p1, corner1) && checkStraightLine(corner1, p2)) {
            return true;
        }
        Point corner2(p2.x, p1.y);
        if (isEmpty(corner2.y, corner2.x) &&
            checkStraightLine(p1, corner2) && checkStraightLine(corner2, p2)) {
            return true;
        }
        return false;
    }

    // 两个拐点：中间线可落在外框（下标 -1 与 COLS/ROWS）以支持绕框连接
    bool checkTwoTurns(const Point& p1, const Point& p2) const {
        // 竖直中间线（枚举列 x，含左右外框）
        for (int x = -1; x <= COLS; ++x) {
            if (x == p1.x || x == p2.x) continue;
            Point c1(x, p1.y), c2(x, p2.y);
            if (isEmpty(c1.y, c1.x) && isEmpty(c2.y, c2.x) &&
                checkStraightLine(p1, c1) && checkStraightLine(c1, c2) &&
                checkStraightLine(c2, p2)) {
                return true;
            }
        }
        // 水平中间线（枚举行 y，含上下外框）
        for (int y = -1; y <= ROWS; ++y) {
            if (y == p1.y || y == p2.y) continue;
            Point c1(p1.x, y), c2(p2.x, y);
            if (isEmpty(c1.y, c1.x) && isEmpty(c2.y, c2.x) &&
                checkStraightLine(p1, c1) && checkStraightLine(c1, c2) &&
                checkStraightLine(c2, p2)) {
                return true;
            }
        }
        return false;
    }

    // ---------- 消除（点击）----------
    // 本轮所有方块对基于同一快照且互不重叠；消除只会把格子变空，
    // 只会“打开”而非“堵住”其它对的路径，故整批点击是安全的。
    void eliminatePairs() {
        if (!IsWindow(windowHandle_)) return;
        SetForegroundWindow(windowHandle_);
        SetWindowPos(windowHandle_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
        for (const auto& pair : pairs_) {
            click(pair.first.point.x, pair.first.point.y);
            click(pair.second.point.x, pair.second.point.y);
        }
    }

    void click(int col, int row) const {
        RECT rect;
        if (!IsWindow(windowHandle_) || !GetWindowRect(windowHandle_, &rect)) return;

        int px = rect.left + CLICK_ORIGIN_X + col * CELL_W;
        int py = rect.top + CLICK_ORIGIN_Y + row * CELL_H;

        SetCursorPos(px, py);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        if (CLICK_GAP_MS) Sleep(CLICK_GAP_MS);
    }

    static bool isKeyPressed(int key) {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);  // 让中文控制台输出正确显示
    Link link;
    link.run();
    return 0;
}
