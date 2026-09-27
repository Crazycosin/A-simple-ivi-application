/*
最小的谷歌cpp代码风格示例
文件名：google_cpp_style_example.cpp
类名：GoogleCppStyleExample
函数名：Run
变量名：kExampleString
常量名：kExampleString
枚举名：kExampleEnum
宏名：kExampleMacro
*/

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

// 类型（类、结构体、typedef、using 别名、枚举名、类型模板参数）命名：
// 大驼峰，无下划线。这里作为 GoogleCppStyleExample 持有的成员类型示例。
class Example {
 public:
  explicit Example(int id) : m_id_(id) {}

  // 取值函数：直接使用小写的值名称，不加 Get 前缀。
  int id() const { return m_id_; }

 private:
  // 私有成员变量：m_aa_ 格式（m_ 前缀 + 下划线结尾）。
  int m_id_;
};

class GoogleCppStyleExample {
 public:
  GoogleCppStyleExample()
      : m_count(0),
        m_example_b(1),
        m_name_("google_cpp_style_example"),
        m_example_a_(2),
        m_example_c_ptr_(std::make_shared<Example>(3)) {}

  // 成员函数命名：大小写混写，前面是动作、后面是主体。
  void SetName(const std::string& name) { m_name_ = name; }

  // 单纯取值函数：直接用小写值名称，不加 Get 前缀。
  const std::string& name() const { return m_name_; }

  void class_struct_name_style() {
    const std::string kExampleString =
        "规则：大驼峰，无下划线\n"
        "示例：UrlTable、PropertiesMap、UrlTableErrors";
    PrintSection(1, "类型 / 结构体 / 枚举 / 类型模板参数命名", kExampleString);
  }

  void file_name_style() {
    const std::string kExampleString =
        "规则：全小写 + 下划线，扩展名使用 .cc / .h（或 .cpp / .hpp）\n"
        "示例：foo_bar.cc ↔ foo_bar.h ↔ class FooBar";
    PrintSection(2, "文件命名", kExampleString);
  }

  void function_name_style() {
    const std::string kExampleString =
        "规则：大驼峰；缩写视作一个词；单纯的取值函数不受此限制\n"
        "示例：AddTableEntry()、StartRpc()（不是 StartRPC）";
    PrintSection(3, "函数命名", kExampleString);
  }

  void variable_name_style() {
    const std::string kExampleString =
        "规则：全小写，aa_bb 格式，aa/bb 取变量完整词意的一部分\n"
        "示例：table_name";
    PrintSection(4, "变量命名", kExampleString);
  }

  void private_member_variable_name_style() {
    const std::string kExampleString =
        "规则：全小写，m_aa_ 格式（m_ 前缀 + 下划线结尾）\n"
        "示例：m_name_";
    PrintSection(5, "私有成员变量命名", kExampleString);
  }

  void public_member_variable_name_style() {
    const std::string kExampleString =
        "规则：全小写，m_bb 格式（m_ 前缀，无下划线结尾）\n"
        "示例：m_count";
    PrintSection(6, "公共成员变量命名", kExampleString);
  }

  void member_function_name_style() {
    const std::string kExampleString =
        "规则：大小写混写，前面是动作、后面是主体；单纯取值函数直接用小写"
        "值名称，不加 Get 前缀\n"
        "示例：SetName(\"x\") 用于设置；name() 用于取值";
    PrintSection(7, "成员函数命名", kExampleString);
  }

  void namespace_name_style() {
    const std::string kExampleString =
        "规则：全小写，顶层命名空间使用项目名或团队名，不缩写\n"
        "示例：websearch::index";
    PrintSection(8, "命名空间命名", kExampleString);
  }

  void enum_name_style() {
    const std::string kExampleString =
        "规则：枚举类型名同类型命名（大驼峰）；枚举值优先使用常量风格 "
        "k + 大驼峰\n"
        "示例：kErrorOutOfMemory（旧代码里的 OUT_OF_MEMORY 不必强改）";
    PrintSection(9, "枚举命名", kExampleString);
  }

  void const_variable_name_style() {
    const std::string kExampleString =
        "规则：constexpr/const 且运行期不变、静态存储期的变量，一律 "
        "k + 大驼峰\n"
        "示例：kDaysInAWeek、kMaxRetry";
    PrintSection(10, "常量命名", kExampleString);
  }

  void raii_object_style() {
    const std::string kExampleString =
        "5.1 所有权与智能指针 —— 资源必须有单一、固定的所有者\n"
        "2.5 静态和全局变量 —— RAII 对象不能具有静态存储期\n"
        "3.1 构造函数的内部操作 —— 不能做“可能失败又无法报错”的初始化\n"
        "3.3 可拷贝类型和可移动类型 —— 作用域型 RAII 类型必须显式禁拷贝\n"
        "4.1 输入和输出 —— 生命周期要求必须写进函数签名和文档\n"
        "6.6 异常 —— RAII 本身不承担异常安全职责";
    PrintSection(11, "RAII 对象约束", kExampleString);
  }

  void standard_cpp_requirements() {
    const std::string kExampleString = "规则：使用 C++17 及以上标准，除非有特殊限制";
    PrintSection(12, "语言标准", kExampleString);
  }

  void smart_pointer_name_style() {
    const std::string kExampleString =
        "规则：优先使用 std::shared_ptr 和 std::unique_ptr 管理动态分配的"
        "资源，避免裸指针 new/delete";
    PrintSection(13, "资源管理 / 智能指针", kExampleString);
  }

  void pointer_variable_name_style() {
    const std::string kExampleString =
        "规则：以 _ptr 结尾，前缀取变量名可取部分\n"
        "示例：m_example_c_ptr_";
    PrintSection(14, "指针变量命名", kExampleString);
  }

  void define_name_style() {
    const std::string kExampleString =
        "规则：全大写 + 下划线（前提是真的躲不开宏）\n"
        "示例：MY_MACRO_THAT_SCARES_SMALL_CHILDREN";
    PrintSection(15, "宏命名", kExampleString);
  }

  // 函数名：Run。统一入口，依次输出全部风格条目，再演示成员变量与
  // 成员函数的实际用法，最后打印整体小结。
  void Run() {
    PrintBanner();

    class_struct_name_style();
    file_name_style();
    function_name_style();
    variable_name_style();
    private_member_variable_name_style();
    public_member_variable_name_style();
    member_function_name_style();
    namespace_name_style();
    enum_name_style();
    const_variable_name_style();
    raii_object_style();
    standard_cpp_requirements();
    smart_pointer_name_style();
    pointer_variable_name_style();
    define_name_style();

    PrintLiveDemo();
    PrintSummary();
  }

  int m_count;
  Example m_example_b;

 private:
  static void PrintRuleLine(char fill) {
    std::cout << std::string(kLineWidth, fill) << "\n";
  }

  static void PrintBanner() {
    PrintRuleLine('=');
    std::cout << "Google C++ 代码风格示例（GoogleCppStyleExample::Run）\n";
    PrintRuleLine('=');
    std::cout << "\n";
  }

  static void PrintSection(int index, const std::string& title,
                            const std::string& detail) {
    std::cout << "[" << std::setw(2) << std::setfill('0') << index
               << std::setfill(' ') << "] " << title << "\n";
    PrintRuleLine('-');
    std::istringstream stream(detail);
    std::string line;
    while (std::getline(stream, line)) {
      std::cout << "    " << line << "\n";
    }
    std::cout << "\n";
  }

  // 用真实的成员变量与成员函数演示上面各条规则，对应
  // m_count（公共成员变量）、m_example_b（公共成员变量）、
  // m_name_ / name() / SetName()（私有成员变量与成员函数）、
  // m_example_a_、m_example_c_ptr_（智能指针成员变量）。
  void PrintLiveDemo() {
    PrintRuleLine('=');
    std::cout << "成员变量与成员函数演示\n";
    PrintRuleLine('=');
    std::cout << "  m_count                = " << m_count << "\n";
    std::cout << "  m_example_b.id()       = " << m_example_b.id() << "\n";
    std::cout << "  name()                 = " << name() << "\n";
    std::cout << "  m_example_a_.id()      = " << m_example_a_.id() << "\n";
    std::cout << "  m_example_c_ptr_->id() = " << m_example_c_ptr_->id()
               << "\n";

    SetName("aoa_style_demo");
    m_count = 42;
    std::cout << "  -- 调用 SetName(\"aoa_style_demo\")，直接赋值 "
                 "m_count = 42 之后 --\n";
    std::cout << "  name()                 = " << name() << "\n";
    std::cout << "  m_count                = " << m_count << "\n";
    std::cout << "\n";
  }

  void PrintSummary() {
    PrintRuleLine('=');
    std::cout << "整体小结：共演示 15 条命名 / 编码风格规则，"
               << "并用 " << kExampleClassCount << " 个真实类型"
               << "（Example、GoogleCppStyleExample）完成了成员变量与"
               << "成员函数的实际用法。\n";
    PrintRuleLine('=');
  }

  static constexpr int kLineWidth = 64;
  static constexpr int kExampleClassCount = 2;

  std::string m_name_;
  Example m_example_a_;
  std::shared_ptr<Example> m_example_c_ptr_;
};

int main() {
  GoogleCppStyleExample example;
  example.Run();
  return 0;
}
