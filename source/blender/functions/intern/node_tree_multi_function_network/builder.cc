#include "builder.h"

namespace FN {

using BLI::ScopedVector;

FunctionTreeMFNetworkBuilder::FunctionTreeMFNetworkBuilder(
    const FunctionNodeTree &function_tree,
    const PreprocessedVTreeMFData &preprocessed_function_tree_data,
    const VTreeMultiFunctionMappings &function_tree_mappings,
    ResourceCollector &resources)
    : m_function_tree(function_tree),
      m_preprocessed_function_tree_data(preprocessed_function_tree_data),
      m_function_tree_mappings(function_tree_mappings),
      m_resources(resources),
      m_socket_by_fsocket(function_tree.socket_count()),
      m_builder(BLI::make_unique<MFNetworkBuilder>())
{
}

MFBuilderFunctionNode &FunctionTreeMFNetworkBuilder::add_function(const MultiFunction &function)
{
  return m_builder->add_function(function);
}

MFBuilderFunctionNode &FunctionTreeMFNetworkBuilder::add_function(const MultiFunction &function,
                                                                  const FNode &fnode)
{
  MFBuilderFunctionNode &node = m_builder->add_function(function);
  this->map_data_sockets(fnode, node);
  return node;
}

MFBuilderDummyNode &FunctionTreeMFNetworkBuilder::add_dummy(const FNode &fnode)
{
  ScopedVector<MFDataType> input_types;
  ScopedVector<StringRef> input_names;
  for (const FInputSocket *fsocket : fnode.inputs()) {
    Optional<MFDataType> data_type = this->try_get_data_type(*fsocket);
    if (data_type.has_value()) {
      input_types.append(data_type.value());
      input_names.append(fsocket->name());
    }
  }

  ScopedVector<MFDataType> output_types;
  ScopedVector<StringRef> output_names;
  for (const FOutputSocket *fsocket : fnode.outputs()) {
    Optional<MFDataType> data_type = this->try_get_data_type(*fsocket);
    if (data_type.has_value()) {
      output_types.append(data_type.value());
      output_names.append(fsocket->name());
    }
  }

  MFBuilderDummyNode &node = m_builder->add_dummy(
      fnode.name(), input_types, output_types, input_names, output_names);
  this->map_data_sockets(fnode, node);
  return node;
}

void FunctionTreeMFNetworkBuilder::map_data_sockets(const FNode &fnode, MFBuilderNode &node)
{
  uint data_inputs = 0;
  for (const FInputSocket *fsocket : fnode.inputs()) {
    if (this->is_data_socket(*fsocket)) {
      this->map_sockets(*fsocket, *node.inputs()[data_inputs]);
      data_inputs++;
    }
  }

  uint data_outputs = 0;
  for (const FOutputSocket *fsocket : fnode.outputs()) {
    if (this->is_data_socket(*fsocket)) {
      this->map_sockets(*fsocket, *node.outputs()[data_outputs]);
      data_outputs++;
    }
  }
}

void FunctionTreeMFNetworkBuilder::assert_fnode_is_mapped_correctly(const FNode &fnode) const
{
  UNUSED_VARS_NDEBUG(fnode);
#ifdef DEBUG
  this->assert_data_sockets_are_mapped_correctly(fnode.inputs().cast<const FSocket *>());
  this->assert_data_sockets_are_mapped_correctly(fnode.outputs().cast<const FSocket *>());
#endif
}

void FunctionTreeMFNetworkBuilder::assert_data_sockets_are_mapped_correctly(
    ArrayRef<const FSocket *> fsockets) const
{
  for (const FSocket *fsocket : fsockets) {
    if (this->is_data_socket(*fsocket)) {
      this->assert_fsocket_is_mapped_correctly(*fsocket);
    }
  }
}

void FunctionTreeMFNetworkBuilder::assert_fsocket_is_mapped_correctly(const FSocket &fsocket) const
{
  BLI_assert(this->fsocket_is_mapped(fsocket));
  MFDataType fsocket_type = this->try_get_data_type(fsocket).value();
  UNUSED_VARS_NDEBUG(fsocket_type);

  if (fsocket.is_input()) {
    for (MFBuilderInputSocket *socket : this->lookup_socket(fsocket.as_input())) {
      MFDataType socket_type = socket->data_type();
      BLI_assert(socket_type == fsocket_type);
      UNUSED_VARS_NDEBUG(socket_type);
    }
  }
  else {
    MFBuilderSocket &socket = this->lookup_socket(fsocket.as_output());
    MFDataType socket_type = socket.data_type();
    BLI_assert(socket_type == fsocket_type);
    UNUSED_VARS_NDEBUG(socket_type);
  }
}

bool FunctionTreeMFNetworkBuilder::has_data_sockets(const FNode &fnode) const
{
  for (const FInputSocket *fsocket : fnode.inputs()) {
    if (this->is_data_socket(*fsocket)) {
      return true;
    }
  }
  for (const FOutputSocket *fsocket : fnode.outputs()) {
    if (this->is_data_socket(*fsocket)) {
      return true;
    }
  }
  return false;
}

const CPPType &FunctionTreeMFNetworkBuilder::cpp_type_from_property(const FNode &fnode,
                                                                    StringRefNull prop_name) const
{
  char *type_name = RNA_string_get_alloc(fnode.rna(), prop_name.data(), nullptr, 0);
  const CPPType &type = this->cpp_type_by_name(type_name);
  MEM_freeN(type_name);
  return type;
}

MFDataType FunctionTreeMFNetworkBuilder::data_type_from_property(const FNode &fnode,
                                                                 StringRefNull prop_name) const
{
  char *type_name = RNA_string_get_alloc(fnode.rna(), prop_name.data(), nullptr, 0);
  MFDataType type = m_function_tree_mappings.data_type_by_type_name.lookup(type_name);
  MEM_freeN(type_name);
  return type;
}

Vector<bool> FNodeMFNetworkBuilder::get_list_base_variadic_states(StringRefNull prop_name)
{
  Vector<bool> states;
  RNA_BEGIN (m_fnode.rna(), itemptr, prop_name.data()) {
    int state = RNA_enum_get(&itemptr, "state");
    if (state == 0) {
      /* single value case */
      states.append(false);
    }
    else if (state == 1) {
      /* list case */
      states.append(true);
    }
    else {
      BLI_assert(false);
    }
  }
  RNA_END;
  return states;
}

void FNodeMFNetworkBuilder::set_matching_fn(const MultiFunction &fn)
{
  MFBuilderFunctionNode &node = m_network_builder.add_function(fn);
  m_network_builder.map_data_sockets(m_fnode, node);
}

const MultiFunction &FNodeMFNetworkBuilder::get_vectorized_function(
    const MultiFunction &base_function, ArrayRef<const char *> is_vectorized_prop_names)
{
  ScopedVector<bool> input_is_vectorized;
  for (const char *prop_name : is_vectorized_prop_names) {
    char state[5];
    RNA_string_get(m_fnode.rna(), prop_name, state);
    BLI_assert(STREQ(state, "BASE") || STREQ(state, "LIST"));

    bool is_vectorized = STREQ(state, "LIST");
    input_is_vectorized.append(is_vectorized);
  }

  if (input_is_vectorized.contains(true)) {
    return this->construct_fn<MF_SimpleVectorize>(base_function, input_is_vectorized);
  }
  else {
    return base_function;
  }
}

std::unique_ptr<FunctionTreeMFNetwork> FunctionTreeMFNetworkBuilder::build()
{
  // m_builder->to_dot__clipboard();

  auto network = BLI::make_unique<MFNetwork>(std::move(m_builder));

  Array<uint> fsocket_by_socket(network->socket_ids().size(), IdMultiMap_UNMAPPED);
  for (uint fsocket_id : IndexRange(m_function_tree.socket_count())) {
    ArrayRef<uint> mapped_ids = m_socket_by_fsocket.lookup(fsocket_id);
    for (uint mapped_id : mapped_ids) {
      fsocket_by_socket[mapped_id] = fsocket_id;
    }
  }

  InlinedTreeMFSocketMap socket_map(
      m_function_tree, *network, std::move(m_socket_by_fsocket), std::move(fsocket_by_socket));

  return BLI::make_unique<FunctionTreeMFNetwork>(
      m_function_tree, std::move(network), std::move(socket_map));
}

}  // namespace FN