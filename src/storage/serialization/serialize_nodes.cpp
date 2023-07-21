//===----------------------------------------------------------------------===//
// This file is automatically generated by scripts/generate_serialization.py
// Do not edit this file manually, your changes will be overwritten
//===----------------------------------------------------------------------===//

#include "duckdb/common/serializer/format_serializer.hpp"
#include "duckdb/common/serializer/format_deserializer.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/extra_type_info.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/planner/tableref/bound_pivotref.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/parser/parsed_data/vacuum_info.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/multi_file_reader_options.hpp"

namespace duckdb {

void BoundCaseCheck::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("when_expr", *when_expr);
	serializer.WriteProperty("then_expr", *then_expr);
}

BoundCaseCheck BoundCaseCheck::FormatDeserialize(FormatDeserializer &deserializer) {
	BoundCaseCheck result;
	deserializer.ReadProperty("when_expr", result.when_expr);
	deserializer.ReadProperty("then_expr", result.then_expr);
	return result;
}

void BoundOrderByNode::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("type", type);
	serializer.WriteProperty("null_order", null_order);
	serializer.WriteProperty("expression", *expression);
}

BoundOrderByNode BoundOrderByNode::FormatDeserialize(FormatDeserializer &deserializer) {
	auto type = deserializer.ReadProperty<OrderType>("type");
	auto null_order = deserializer.ReadProperty<OrderByNullType>("null_order");
	auto expression = deserializer.ReadProperty<unique_ptr<Expression>>("expression");
	BoundOrderByNode result(type, null_order, std::move(expression));
	return result;
}

void BoundParameterData::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("value", value);
	serializer.WriteProperty("return_type", return_type);
}

shared_ptr<BoundParameterData> BoundParameterData::FormatDeserialize(FormatDeserializer &deserializer) {
	auto value = deserializer.ReadProperty<Value>("value");
	auto result = duckdb::shared_ptr<BoundParameterData>(new BoundParameterData(value));
	deserializer.ReadProperty("return_type", result->return_type);
	return result;
}

void BoundPivotInfo::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("group_count", group_count);
	serializer.WriteProperty("types", types);
	serializer.WriteProperty("pivot_values", pivot_values);
	serializer.WriteProperty("aggregates", aggregates);
}

BoundPivotInfo BoundPivotInfo::FormatDeserialize(FormatDeserializer &deserializer) {
	BoundPivotInfo result;
	deserializer.ReadProperty("group_count", result.group_count);
	deserializer.ReadProperty("types", result.types);
	deserializer.ReadProperty("pivot_values", result.pivot_values);
	deserializer.ReadProperty("aggregates", result.aggregates);
	return result;
}

void CaseCheck::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("when_expr", *when_expr);
	serializer.WriteProperty("then_expr", *then_expr);
}

CaseCheck CaseCheck::FormatDeserialize(FormatDeserializer &deserializer) {
	CaseCheck result;
	deserializer.ReadProperty("when_expr", result.when_expr);
	deserializer.ReadProperty("then_expr", result.then_expr);
	return result;
}

void ColumnBinding::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("table_index", table_index);
	serializer.WriteProperty("column_index", column_index);
}

ColumnBinding ColumnBinding::FormatDeserialize(FormatDeserializer &deserializer) {
	ColumnBinding result;
	deserializer.ReadProperty("table_index", result.table_index);
	deserializer.ReadProperty("column_index", result.column_index);
	return result;
}

void ColumnDefinition::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("name", name);
	serializer.WriteProperty("type", type);
	serializer.WriteOptionalProperty("expression", expression);
	serializer.WriteProperty("category", category);
	serializer.WriteProperty("compression_type", compression_type);
}

ColumnDefinition ColumnDefinition::FormatDeserialize(FormatDeserializer &deserializer) {
	auto name = deserializer.ReadProperty<string>("name");
	auto type = deserializer.ReadProperty<LogicalType>("type");
	auto expression = deserializer.ReadOptionalProperty<unique_ptr<ParsedExpression>>("expression");
	auto category = deserializer.ReadProperty<TableColumnType>("category");
	ColumnDefinition result(std::move(name), std::move(type), std::move(expression), category);
	deserializer.ReadProperty("compression_type", result.compression_type);
	return result;
}

void ColumnList::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("columns", columns);
}

ColumnList ColumnList::FormatDeserialize(FormatDeserializer &deserializer) {
	auto columns = deserializer.ReadProperty<vector<ColumnDefinition>>("columns");
	ColumnList result(std::move(columns));
	return result;
}

void CommonTableExpressionInfo::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("aliases", aliases);
	serializer.WriteProperty("query", *query);
	serializer.WriteProperty("materialized", materialized);
}

unique_ptr<CommonTableExpressionInfo> CommonTableExpressionInfo::FormatDeserialize(FormatDeserializer &deserializer) {
	auto result = duckdb::unique_ptr<CommonTableExpressionInfo>(new CommonTableExpressionInfo());
	deserializer.ReadProperty("aliases", result->aliases);
	deserializer.ReadProperty("query", result->query);
	deserializer.ReadProperty("materialized", result->materialized);
	return result;
}

void CommonTableExpressionMap::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("map", map);
}

CommonTableExpressionMap CommonTableExpressionMap::FormatDeserialize(FormatDeserializer &deserializer) {
	CommonTableExpressionMap result;
	deserializer.ReadProperty("map", result.map);
	return result;
}

void JoinCondition::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("left", *left);
	serializer.WriteProperty("right", *right);
	serializer.WriteProperty("comparison", comparison);
}

JoinCondition JoinCondition::FormatDeserialize(FormatDeserializer &deserializer) {
	JoinCondition result;
	deserializer.ReadProperty("left", result.left);
	deserializer.ReadProperty("right", result.right);
	deserializer.ReadProperty("comparison", result.comparison);
	return result;
}

void LogicalType::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("id", id_);
	serializer.WriteOptionalProperty("type_info", type_info_);
}

LogicalType LogicalType::FormatDeserialize(FormatDeserializer &deserializer) {
	auto id = deserializer.ReadProperty<LogicalTypeId>("id");
	auto type_info = deserializer.ReadOptionalProperty<shared_ptr<ExtraTypeInfo>>("type_info");
	LogicalType result(id, std::move(type_info));
	return result;
}

void MultiFileReaderOptions::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("filename", filename);
	serializer.WriteProperty("hive_partitioning", hive_partitioning);
	serializer.WriteProperty("auto_detect_hive_partitioning", auto_detect_hive_partitioning);
	serializer.WriteProperty("union_by_name", union_by_name);
	serializer.WriteProperty("hive_types_autocast", hive_types_autocast);
	serializer.WriteProperty("hive_types_schema", hive_types_schema);
}

MultiFileReaderOptions MultiFileReaderOptions::FormatDeserialize(FormatDeserializer &deserializer) {
	MultiFileReaderOptions result;
	deserializer.ReadProperty("filename", result.filename);
	deserializer.ReadProperty("hive_partitioning", result.hive_partitioning);
	deserializer.ReadProperty("auto_detect_hive_partitioning", result.auto_detect_hive_partitioning);
	deserializer.ReadProperty("union_by_name", result.union_by_name);
	deserializer.ReadProperty("hive_types_autocast", result.hive_types_autocast);
	deserializer.ReadProperty("hive_types_schema", result.hive_types_schema);
	return result;
}

void OrderByNode::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("type", type);
	serializer.WriteProperty("null_order", null_order);
	serializer.WriteProperty("expression", *expression);
}

OrderByNode OrderByNode::FormatDeserialize(FormatDeserializer &deserializer) {
	auto type = deserializer.ReadProperty<OrderType>("type");
	auto null_order = deserializer.ReadProperty<OrderByNullType>("null_order");
	auto expression = deserializer.ReadProperty<unique_ptr<ParsedExpression>>("expression");
	OrderByNode result(type, null_order, std::move(expression));
	return result;
}

void PivotColumn::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("pivot_expressions", pivot_expressions);
	serializer.WriteProperty("unpivot_names", unpivot_names);
	serializer.WriteProperty("entries", entries);
	serializer.WriteProperty("pivot_enum", pivot_enum);
}

PivotColumn PivotColumn::FormatDeserialize(FormatDeserializer &deserializer) {
	PivotColumn result;
	deserializer.ReadProperty("pivot_expressions", result.pivot_expressions);
	deserializer.ReadProperty("unpivot_names", result.unpivot_names);
	deserializer.ReadProperty("entries", result.entries);
	deserializer.ReadProperty("pivot_enum", result.pivot_enum);
	return result;
}

void SampleOptions::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("sample_size", sample_size);
	serializer.WriteProperty("is_percentage", is_percentage);
	serializer.WriteProperty("method", method);
	serializer.WriteProperty("seed", seed);
}

unique_ptr<SampleOptions> SampleOptions::FormatDeserialize(FormatDeserializer &deserializer) {
	auto result = duckdb::unique_ptr<SampleOptions>(new SampleOptions());
	deserializer.ReadProperty("sample_size", result->sample_size);
	deserializer.ReadProperty("is_percentage", result->is_percentage);
	deserializer.ReadProperty("method", result->method);
	deserializer.ReadProperty("seed", result->seed);
	return result;
}

void TableFilterSet::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("filters", filters);
}

TableFilterSet TableFilterSet::FormatDeserialize(FormatDeserializer &deserializer) {
	TableFilterSet result;
	deserializer.ReadProperty("filters", result.filters);
	return result;
}

void VacuumOptions::FormatSerialize(FormatSerializer &serializer) const {
	serializer.WriteProperty("vacuum", vacuum);
	serializer.WriteProperty("analyze", analyze);
}

VacuumOptions VacuumOptions::FormatDeserialize(FormatDeserializer &deserializer) {
	VacuumOptions result;
	deserializer.ReadProperty("vacuum", result.vacuum);
	deserializer.ReadProperty("analyze", result.analyze);
	return result;
}

} // namespace duckdb
