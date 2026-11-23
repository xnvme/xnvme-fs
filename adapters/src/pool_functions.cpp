// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#include "xnvme_fs_filesystem.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace xnvme_fs {

struct PoolBindData : public TableFunctionData {
	string name;
};

struct PoolState : public GlobalTableFunctionState {
	string name;
	::xnvme::fs::PoolInfo info;
	idx_t next = 0;
};

static unique_ptr<FunctionData> PoolBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto data = make_uniq<PoolBindData>();
	data->name = input.inputs[0].ToString();
	names = {"pool",         "member_index",   "member",      "size_bytes", "extents",
	         "free_extents", "layout_version", "extent_size", "generation", "files"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::INTEGER, LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::BIGINT};
	return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> InfoInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<PoolBindData>();
	ClientContextFileOpener opener(context);
	auto state = make_uniq<PoolState>();
	try {
		state->name = data.name;
		state->info = GetSession(data.name, &opener)->Info();
	} catch (std::system_error &err) {
		Raise(err);
	}
	return std::move(state);
}

static void PoolScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PoolState>();
	auto &s = state.info;
	idx_t count = 0;
	while (state.next < s.members.size() && count < STANDARD_VECTOR_SIZE) {
		auto i = state.next++;
		output.SetValue(0, count, Value(state.name));
		output.SetValue(1, count, Value::BIGINT(int64_t(i)));
		output.SetValue(2, count, Value(string(s.members[i].path)));
		output.SetValue(3, count, Value::BIGINT(int64_t(s.members[i].size)));
		output.SetValue(4, count, Value::BIGINT(int64_t(s.members[i].extents)));
		output.SetValue(5, count, Value::BIGINT(int64_t(s.members[i].free_extents)));
		output.SetValue(6, count, Value::INTEGER(int32_t(s.pool.layout_version)));
		output.SetValue(7, count, Value::BIGINT(int64_t(s.pool.extent_size)));
		output.SetValue(8, count, Value::BIGINT(int64_t(s.pool.generation)));
		output.SetValue(9, count, Value::BIGINT(int64_t(s.pool.files)));
		count++;
	}
	output.SetCardinality(count);
}

void RegisterPoolFunctions(ExtensionLoader &loader) {
	TableFunction info("xnvme_fs_pool_info", {LogicalType::VARCHAR}, PoolScan, PoolBind, InfoInit);
	loader.RegisterFunction(info);
}

} // namespace xnvme_fs
} // namespace duckdb
