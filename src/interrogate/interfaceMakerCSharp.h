/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file interfaceMakerCSharp.h
 * @date 2026-04-09
 */

#ifndef INTERFACEMAKERCSHARP_H
#define INTERFACEMAKERCSHARP_H

#include "dtoolbase.h"

#include "interfaceMaker.h"
#include "interrogate_interface.h"

#include <deque>
#include <fstream>
#include <map>
#include <set>

class FunctionRemap;
class Filename;
class InterrogateFunctionWrapper;

extern std::set<int> csharp_owned_type_indices;
extern std::map<int, std::string> csharp_type_module_map;
extern std::map<std::string, std::string> csharp_library_to_module;

class InterfaceMakerCSharp : public InterfaceMaker {
public:
  InterfaceMakerCSharp(InterrogateModuleDef *def);
  virtual ~InterfaceMakerCSharp();

  virtual void write_prototypes(std::ostream &out, std::ostream *out_h) override;
  virtual void write_functions(std::ostream &out) override;
  virtual void write_module_support(std::ostream &out, std::ostream *out_h,
                                    InterrogateModuleDef *def) override;
  virtual void write_module(std::ostream &out, std::ostream *out_h,
                            InterrogateModuleDef *def) override;

  virtual bool synthesize_this_parameter() override;

  // Marks a command-line database file as already loaded so that
  // load_all_search_dir_databases() will not re-load it.
  void mark_database_loaded(const Filename &database_file);

protected:
  virtual std::string get_wrapper_prefix() override;
  virtual std::string get_unique_prefix() override;

  virtual void
  record_function_wrapper(InterrogateFunction &ifunc,
                          FunctionWrapperIndex wrapper_index) override;

private:
  void write_csharp_files(InterrogateModuleDef *def);
  void write_support_file(const std::string &dir, const std::string &cs_namespace);
  void write_native_methods_file(const std::string &dir, const std::string &cs_namespace);
  void record_secondary_base_members();

  void write_enum_files(const std::string &dir, const std::string &cs_namespace);
  void write_enum_type(std::ostream &out, const InterrogateType &itype);

  void write_class_files(const std::string &dir, const std::string &cs_namespace);
  void write_class_file(const std::string &dir, const std::string &cs_namespace,
                        Object *object);
  void write_collection_adapter_class(std::ostream &out, Object *object);

  void write_interface(std::ostream &out, Object *object);
  void write_proxy_class(std::ostream &out, const std::string &cs_namespace,
                         Object *object);

  void write_constructor(std::ostream &out, Function *func, Object *object,
                         int indent_level);
  void write_method(std::ostream &out, Function *func, Object *object,
                    int indent_level, bool is_interface,
                    std::set<std::string> *emitted_signatures = nullptr);
  void write_operator_aliases(std::ostream &out, Object *object,
                              int indent_level,
                              std::set<std::string> *emitted_signatures,
                              bool is_interface);
  void write_property(std::ostream &out, Property *prop, Object *object,
                      int indent_level, bool is_interface);
  void write_property_from_wrapper(std::ostream &out,
                                   const InterrogateElement &ielement,
                                   Object *object, int indent_level,
                                   bool is_interface);
  void write_sequence_property_from_wrapper(std::ostream &out,
                                            const InterrogateElement &ielement,
                                            Object *object, int indent_level,
                                            bool is_interface);
  void write_map_property_from_wrapper(std::ostream &out,
                                       const InterrogateElement &ielement,
                                       Object *object, int indent_level,
                                       bool is_interface);
  void write_dispose_pattern(std::ostream &out, Object *object,
                             int indent_level);

  void write_globals_file(const std::string &dir, const std::string &cs_namespace,
                          InterrogateModuleDef *def);

  void write_dllimport(std::ostream &out, FunctionRemap *remap,
                       const std::string &friendly_name);
  void write_dllimport(std::ostream &out, const InterrogateFunction &ifunc,
                       const InterrogateFunctionWrapper &wrapper,
                       const std::string &friendly_name);

  std::string get_c_wrapper_name(FunctionRemap *remap) const;
  std::string get_pinvoke_name(Function *func, FunctionRemap *remap) const;
  std::string get_pinvoke_name(const InterrogateFunction &ifunc,
                               const InterrogateFunctionWrapper &wrapper) const;
  bool is_current_native_methods_type(const InterrogateType &itype) const;
  bool is_current_native_methods_type(TypeIndex tidx, const InterrogateType &itype) const;
  std::string get_native_methods_class_name(const InterrogateType &itype) const;
  std::string get_pinvoke_call_name(Function *func, FunctionRemap *remap) const;
  std::string get_pinvoke_call_name(const InterrogateFunction &ifunc,
                                    const InterrogateFunctionWrapper &wrapper) const;
  std::string get_native_this_argument(Object *object, Function *func,
                                       FunctionRemap *remap) const;
  std::string get_native_this_argument(const InterrogateType &object_type,
                                       const InterrogateFunction &ifunc) const;
  std::string get_upcast_pinvoke_name(const InterrogateType &itype,
                                      int derivation_index) const;
  void get_secondary_base_types(const InterrogateType &itype,
                                std::vector<const InterrogateType *> &bases) const;
  bool ensure_database_loaded(const InterrogateType &itype) const;
  void request_external_database(const Filename &database_file) const;
  bool find_upcast_chain(const InterrogateType &itype, TypeIndex target_type,
                          std::vector<std::string> &pinvoke_names,
                          std::set<TypeIndex> &visited) const;

  std::string get_pinvoke_type(CPPType *type, bool for_return = false) const;
  std::string get_pinvoke_type(TypeIndex type, bool for_return = false) const;
  std::string get_csharp_type(CPPType *type, bool for_return = false) const;
  std::string get_csharp_type(TypeIndex type, bool for_return = false) const;
  std::string get_csharp_type_for_wrapper(ParameterRemap *remap,
                                          bool for_return = false) const;
  const InterrogateType *find_csharp_object_type(const std::string &type_name) const;
  std::string globalize_class_name(const InterrogateType &itype) const;
  std::string csharp_out_parameter_type(TypeIndex type_index) const;
  std::string get_csharp_signature_type(CPPType *type,
                                        bool for_return = false) const;
  // `is_parameter` marks the type as appearing in an incoming parameter rather
  // than a return.  (`for_return` is a misnomer: every call site passes the
  // type's NULLABILITY through it, not its position.)  Const collection facades
  // need the distinction -- see get_csharp_signature_type.
  std::string get_csharp_signature_type(TypeIndex type,
                                        bool for_return = false,
                                        bool is_parameter = false) const;
  std::string get_csharp_signature_type_for_wrapper(ParameterRemap *remap,
                                                    bool for_return = false) const;
  std::string get_csharp_native_object_class_name(CPPType *type) const;
  std::string get_csharp_native_object_class_name(TypeIndex type) const;
  std::string get_marshal_attribute(CPPType *type, bool for_return = false) const;
  std::string get_marshal_attribute(TypeIndex type, bool for_return = false) const;

  std::string get_class_name(const InterrogateType &itype) const;
  std::string get_interface_name(const InterrogateType &itype) const;
  std::string get_qualified_class_name(const InterrogateType &itype) const;
  std::string get_qualified_interface_name(const InterrogateType &itype) const;
  bool uses_csharp_interface(const InterrogateType &itype) const;
  void rebuild_csharp_interface_cache() const;
  bool mark_csharp_interface_base_chain(TypeIndex type_index,
                                        std::set<TypeIndex> &visited) const;
  std::string get_public_native_object_type_name(const InterrogateType &itype,
                                                 bool for_return) const;

  // Nested-type support. get_class_name / get_interface_name return the FLAT
  // name (filenames, P/Invoke names, opaque helpers, collision keys); the helpers
  // below give the simple (definition) and dotted (reference) forms.
  bool should_nest_type(const InterrogateType &itype) const;
  void get_outer_class_chain(const InterrogateType &itype,
                             std::vector<const InterrogateType *> &out) const;
  std::string get_flat_display_name(const InterrogateType &itype) const;
  std::string get_simple_class_name(const InterrogateType &itype) const;
  std::string get_nested_class_name(const InterrogateType &itype) const;
  std::string get_simple_interface_name(const InterrogateType &itype) const;
  std::string get_nested_interface_name(const InterrogateType &itype) const;
  int open_nesting_wrappers(std::ostream &out, const InterrogateType &itype) const;
  void close_nesting_wrappers(std::ostream &out, int count) const;
  std::string get_type_module_name(const InterrogateType &itype) const;
  std::string get_base_class_clause(const InterrogateType &itype) const;
  std::string get_interface_list(const InterrogateType &itype) const;
  std::string get_interface_base_list(const InterrogateType &itype) const;
  std::string get_collection_interface_type(const InterrogateType &itype,
                                            bool for_return) const;
  std::string get_collection_element_type(const InterrogateType &itype,
                                          bool for_signature) const;
  std::string get_collection_element_type_from_cpp_name(const std::string &cpp_name,
                                                        bool for_signature) const;
  std::string get_collection_element_cpp_type(const InterrogateType &itype) const;
  std::string get_collection_helper_name(const InterrogateType &itype,
                                         const std::string &op) const;

  int inherited_method_signature_kind(const InterrogateType &itype,
                                      const std::string &method_name,
                                      const std::vector<std::string> &param_types,
                                      const std::string &return_type,
                                      bool is_static,
                                      bool primary_chain_only);

  bool is_wrapped_type(TypeIndex type_index) const;
  bool type_has_destructor(Object *object) const;
  std::string get_destructor_wrapper_name(Object *object) const;

  std::string _dll_name;
  std::string _current_library_name;
  std::string _current_module_name;

  mutable std::set<std::string> _loaded_external_databases;
  mutable std::deque<std::string> _external_database_paths;
  mutable std::deque<InterrogateModuleDef> _external_database_requests;
  mutable bool _csharp_interface_cache_valid;
  mutable int _csharp_interface_cache_type_count;
  mutable std::set<TypeIndex> _csharp_interface_type_indices;
  std::set<std::string> _written_enums;
  // Lowercase class names from every module in the global database, used for
  // case-insensitive interface-name collision detection across modules.
  mutable std::set<std::string> _all_class_names_lower;

  void load_all_search_dir_databases();

};

#endif
