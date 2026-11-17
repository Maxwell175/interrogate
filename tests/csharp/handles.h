//FLAGS: -promiscuous -string
#include <string>
#include <vector>

// The C wrappers pass TypeHandle and ButtonHandle -- the two types
// TypeManager::is_handle() names -- as their integer index rather than by
// pointer, so the managed wrapper of one holds an index where any other holds
// a pointer.  These are cut-down copies with the same shape: one int member,
// a constructor from it and get_index() back.

// Exported at module scope so the driver can tell the names apart.
int handles_button_count();

// How many times the C++ == or != of either handle has run, so the driver can
// tell an index compared in C# from one compared here.
int handles_equality_calls();

class TypeHandle {
public:
  constexpr TypeHandle() = default;
  static TypeHandle from_index(int index);
  int get_index() const;
  std::string get_name() const;

  bool operator == (const TypeHandle &other) const;
  bool operator != (const TypeHandle &other) const;

private:
  int _index = 0;
};

class ButtonHandle {
public:
  ButtonHandle() = default;
  explicit ButtonHandle(int index);
  ButtonHandle(const std::string &name);

  int get_index() const;
  std::string get_name() const;
  bool matches(const ButtonHandle &other) const;
  int compare_to(const ButtonHandle &other) const;
  ButtonHandle get_alias() const;

  bool operator == (const ButtonHandle &other) const;
  bool operator != (const ButtonHandle &other) const;
  bool operator < (const ButtonHandle &other) const;

private:
  int _index = 0;
};

// Takes and gives handles by value, by const reference, by pointer and by
// non-const reference.
class ButtonList {
public:
  ButtonList();
  explicit ButtonList(const ButtonHandle &first);
  // Only methods pass a handle through a temporary; this is not bound.
  ButtonList(ButtonHandle &next, int count);

  void add_button(ButtonHandle button);
  bool add_button_ref(const ButtonHandle &button);
  bool has_button(const ButtonHandle &button) const;
  int get_num_buttons() const;
  ButtonHandle get_button(int n) const;
  const ButtonHandle &get_last_button() const;

  void set_kind(TypeHandle kind);
  TypeHandle get_kind() const;
  bool is_kind(const TypeHandle &kind) const;

  // C# passes these through a temporary: the wrapper builds a handle from
  // the index it is given, and gives back the index the call left.
  void get_first_button(ButtonHandle &result) const;
  bool next_button(ButtonHandle &button) const;
  void clear_button(ButtonHandle *button) const;
  void add_button_ptr(const ButtonHandle *button);
  void next_kind(TypeHandle &kind) const;
  // A C# operator cannot pass one by ref, so this is a method only.
  ButtonList operator + (ButtonHandle &button) const;

private:
  std::vector<ButtonHandle> _buttons;
  TypeHandle _kind;
};
