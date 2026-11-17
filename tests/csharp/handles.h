//FLAGS: -promiscuous -string -DCPPPARSER
#include <string>
#include <vector>

// The C wrappers pass TypeHandle and ButtonHandle -- the two types
// TypeManager::is_handle() names -- as their integer index rather than by
// pointer, so the managed wrapper of one holds an index where any other holds
// a pointer.  These are cut-down copies with the same shape: one int member,
// a constructor from it and get_index() back.

// MAKE_PROPERTY, MAKE_SEQ_PROPERTY and MAKE_MAP_PROPERTY expand to interrogate
// directives under the parser and to nothing for the C++ compiler, as
// dtoolbase.h defines them.
#ifdef CPPPARSER
#define MAKE_PROPERTY(property_name, ...) __make_property(property_name, __VA_ARGS__)
#define MAKE_SEQ_PROPERTY(property_name, ...) __make_seq_property(property_name, __VA_ARGS__)
#define MAKE_MAP_PROPERTY(property_name, ...) __make_map_property(property_name, __VA_ARGS__)
#else
#define MAKE_PROPERTY(property_name, ...)
#define MAKE_SEQ_PROPERTY(property_name, ...)
#define MAKE_MAP_PROPERTY(property_name, ...)
#endif

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
  // Only methods pass a handle through a temporary; these are not bound.
  ButtonList(ButtonHandle &next, int count);
  ButtonList(const ButtonHandle *first, int count, int step);

  void add_button(ButtonHandle button);
  bool add_button_ref(const ButtonHandle &button);
  bool has_button(const ButtonHandle &button) const;
  int get_num_buttons() const;
  ButtonHandle get_button(int n) const;
  // C# returns these as the handle (null for index 0), the others as the index.
  const ButtonHandle &get_last_button() const;
  const ButtonHandle &get_no_button() const;
  const ButtonHandle &get_button_ref(int n) const;
  MAKE_PROPERTY(last_button, get_last_button);
  MAKE_SEQ_PROPERTY(buttons, get_num_buttons, get_button_ref);

  void set_kind(TypeHandle kind);
  TypeHandle get_kind() const;
  bool is_kind(const TypeHandle &kind) const;
  const TypeHandle &get_kind_ref() const;

  // C# passes these through a temporary: the wrapper builds a handle from
  // the index it is given, and gives back the index the call left.
  void get_first_button(ButtonHandle &result) const;
  bool next_button(ButtonHandle &button) const;
  void clear_button(ButtonHandle *button) const;
  void add_button_ptr(const ButtonHandle *button);
  void next_kind(TypeHandle &kind) const;
  // A pointer may be null, and C# passes a null handle on as one.
  bool is_null_button(const ButtonHandle *button) const;
  bool fill_button(ButtonHandle *button) const;
  bool fill_kind(TypeHandle *kind) const;
  // A C# operator cannot pass one by ref, so these are methods only.
  ButtonList operator + (ButtonHandle &button) const;
  ButtonList operator - (const ButtonHandle *button) const;

private:
  std::vector<ButtonHandle> _buttons;
  TypeHandle _kind;
};

// An indexer and a map property over handles returned by const reference:
// none, mouse1 and enter, by position and by name.
class ButtonRow {
public:
  ButtonRow();
  int size() const;
  const ButtonHandle &operator [](int i) const;
  bool has_named(const std::string &name) const;
  const ButtonHandle &get_named(const std::string &name) const;
  MAKE_MAP_PROPERTY(named, has_named, get_named);

private:
  std::vector<ButtonHandle> _buttons;
};
