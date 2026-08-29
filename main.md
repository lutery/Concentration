这是 C++ 的**常量成员函数（const member function）**写法。

## 含义

函数签名末尾的 `const` 修饰的是**隐式的 `this` 指针**，表示这个成员函数**不会修改对象的状态**（即不会修改任何非 `mutable` 成员变量）。

```cpp
bool checkStraightLine(const Point& p1, const Point& p2) const
//                                                     ^^^^^ 修饰 this
```

它等价于把 `this` 指针声明成指向常量的指针：

```cpp
// 编译器视角下的等价形式
bool checkStraightLine(const Point* this, const Point& p1, const Point& p2)
```

## 作用

1. **承诺不修改对象**：函数体内不能修改任何非 `mutable` 成员变量，也不能调用非 const 成员函数。

2. **可以被 const 对象调用**：只有 const 成员函数才能被 `const` 对象或 `const` 引用/指针调用。

```cpp
class Line {
public:
    bool checkStraightLine(const Point& p1, const Point& p2) const;
};

void f(const Line& line) {
    // 如果 checkStraightLine 没有 const，这里会编译报错
    line.checkStraightLine(a, b);  // OK
}
```

## 与参数里的 const 的区别

注意这里有**两个不同的 `const`**：

| 位置 | 修饰对象 | 含义 |
|------|---------|------|
| `const Point& p1` | 参数 `p1` | 参数是常量引用，函数内不能修改 `p1` |
| 末尾的 `const` | `this`（当前对象） | 函数不修改当前对象 |

## 一个常见陷阱

`const` 是函数签名的一部分，可以用于**重载**：

```cpp
class Vec {
public:
    int& operator[](int i) { return data[i]; }        // 非 const 版本
    const int& operator[](int i) const { return data[i]; }  // const 版本
};
```

非 const 对象调用第一个，const 对象调用第二个。

## 总结

末尾的 `const` 表示这是一个**只读的成员函数**——它保证不会改变调用它的那个对象。这是 C++ 里表达"这个函数不会修改对象"的约束，既让代码意图更清晰，也允许该函数被 const 对象调用，是良好实践（尤其是像 `checkStraightLine` 这种纯查询/判断类的函数，通常都应该加 `const`）。

不只是给人看的，它有**实际的编译期约束和功能差异**。区别主要体现在三个方面：

## 1. 编译期强制约束（不只是"约定"）

加了 `const`，编译器会**强制**保证函数体内不能修改成员变量。如果不加，你写错了编译器也不会拦你：

```cpp
class Line {
    int slope_;
public:
    // 加了 const：下面任何修改成员的操作都会编译报错
    bool checkStraightLine(const Point& p1, const Point& p2) const {
        // slope_ = 5;        // ❌ 编译错误：不能修改成员
        // helper();          // ❌ 编译错误：helper 非 const
        return p1.x == p2.x;
    }

    // 不加 const：可以随便改，编译器不管
    bool checkStraightLine2(const Point& p1, const Point& p2) {
        slope_ = 5;           // ✅ 合法（可能是个 bug）
        return p1.x == p2.x;
    }
};
```

所以它不只是"给人看"，而是**让编译器帮你把关**——防止误改对象状态。

## 2. 能否被 const 对象调用（这是最关键的实际区别）

这是最实质的区别。**只有 const 成员函数才能被 const 对象调用**：

```cpp
void analyze(const Line& line, const Point& a, const Point& b) {
    line.checkStraightLine(a, b);   // ✅ 只有加了 const 才能编译
    // line.checkStraightLine2(a, b); // ❌ 编译错误：非 const 函数
}
```

如果你的函数没加 `const`，那么任何持有 `const Line&`、`const Line*` 或 `const Line` 的地方都无法调用它。这会导致：

- 无法在 const 上下文中使用
- 无法配合 STL 容器/算法中返回 const 引用的场景
- 无法在 `const` 成员函数里调用它

## 3. 支持函数重载（const 是签名的一部分）

`const` 参与重载决议，同一个函数名可以有 const 和非 const 两个版本：

```cpp
class Vec {
    int* data_;
public:
    int& operator[](int i) { return data_[i]; }            // 非 const 对象走这里
    const int& operator[](int i) const { return data_[i]; } // const 对象走这里
};

Vec v;
v[0] = 1;              // 调用非 const 版本，返回可写引用

const Vec cv;
int x = cv[0];         // 调用 const 版本，返回只读引用
// cv[0] = 1;          // ❌ 编译错误
```

## 总结对比

| 维度 | 不加 const | 加 const |
|------|-----------|----------|
| 能否修改成员变量 | 能 | 不能（编译期禁止） |
| 能否被 const 对象调用 | **不能** | 能 |
| 能否调用其他非 const 成员函数 | 能 | 不能 |
| 参与重载 | 是（非 const 版本） | 是（const 版本） |
| 编译器保护 | 无 | 有 |

所以核心结论：**它不只是文档性注释，而是有真实的编译期语义**。最重要的实际影响是第 2 点——不加 `const` 的函数无法在 const 对象/引用/指针上调用。对于 `checkStraightLine` 这种纯判断函数，加 `const` 是正确且必要的做法，否则一旦有代码通过 `const Line&` 引用它，就会编译失败。