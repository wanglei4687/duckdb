#include "duckdb/optimizer/deliminator.hpp"

#include "duckdb/planner/operator/logical_delim_join.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

class DeliminatorPlanUpdater : LogicalOperatorVisitor {
public:
	DeliminatorPlanUpdater() {
	}
	//! Update the plan after a DelimGet has been removed
	void VisitOperator(LogicalOperator &op) override;
	void VisitExpression(unique_ptr<Expression> *expression) override;
	//! Whether the operator has one or more children of type DELIM_GET
	bool HasChildDelimGet(LogicalOperator &op);

	expression_map_t<Expression *> expr_map;
	column_binding_map_t<bool> projection_map;
	unique_ptr<LogicalOperator> temp_ptr;
};

void DeliminatorPlanUpdater::VisitOperator(LogicalOperator &op) {
	VisitOperatorChildren(op);
	VisitOperatorExpressions(op);
	// now check if this is a delim join that can be removed
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN && !HasChildDelimGet(op)) {
		auto &delim_join = (LogicalDelimJoin &)op;
		auto decs = &delim_join.duplicate_eliminated_columns;
		for (auto &cond : delim_join.conditions) {
			if (cond.comparison != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			auto &colref = (BoundColumnRefExpression &)*cond.right;
			if (projection_map.find(colref.binding) != projection_map.end()) {
				// value on the right is a projection of removed DelimGet
				for (idx_t i = 0; i < decs->size(); i++) {
					if (decs->at(i)->Equals(cond.left.get())) {
						// the value on the left no longer needs to be a duplicate-eliminated column
						decs->erase(decs->begin() + i);
						break;
					}
				}
				// whether we applied an IS NOT NULL filter
				cond.null_values_are_equal = true; //projection_map[colref.binding];
			}
		}
		// change type if there are no more duplicate-eliminated columns
		if (decs->empty()) {
			delim_join.type = LogicalOperatorType::LOGICAL_COMPARISON_JOIN;
		}
	}
}

void DeliminatorPlanUpdater::VisitExpression(unique_ptr<Expression> *expression) {
	if (expr_map.find(expression->get()) != expr_map.end()) {
		*expression = expr_map[expression->get()]->Copy();
	} else {
		VisitExpressionChildren(**expression);
	}
}

bool DeliminatorPlanUpdater::HasChildDelimGet(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasChildDelimGet(*child)) {
			return true;
		}
	}
	return false;
}

unique_ptr<LogicalOperator> Deliminator::Optimize(unique_ptr<LogicalOperator> op) {
	vector<unique_ptr<LogicalOperator> *> candidates;
	FindCandidates(&op, candidates);

	for (auto candidate : candidates) {
		DeliminatorPlanUpdater updater;
		if (RemoveCandidate(candidate, updater)) {
			updater.VisitOperator(*op);
		}
	}
	return op;
}

void Deliminator::FindCandidates(unique_ptr<LogicalOperator> *op_ptr,
                                 vector<unique_ptr<LogicalOperator> *> &candidates) {
	auto op = op_ptr->get();
	// search children before adding, so the deepest candidates get added first
	for (auto &child : op->children) {
		FindCandidates(&child, candidates);
	}
	if ( // Projection/Aggregate
	    (op->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	     op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) &&
	    // followed by a Join
	    op->children[0]->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    // DelimGet as direct child (left or right)
	    (op->children[0]->children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET ||
	     op->children[0]->children[1]->type == LogicalOperatorType::LOGICAL_DELIM_GET ||
	     // Filter + DelimGet as a child (left or right)
	     (op->children[0]->children[0]->type == LogicalOperatorType::LOGICAL_FILTER &&
	      op->children[0]->children[0]->children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET) ||
	     (op->children[0]->children[1]->type == LogicalOperatorType::LOGICAL_FILTER &&
	      op->children[0]->children[1]->children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET))) {
		candidates.push_back(op_ptr);
	}
}

bool Deliminator::RemoveCandidate(unique_ptr<LogicalOperator> *op_ptr, DeliminatorPlanUpdater &updater) {
	auto &proj_or_agg = **op_ptr;
	auto &join = (LogicalComparisonJoin &)*proj_or_agg.children[0];
	if (join.join_type != JoinType::INNER && join.join_type != JoinType::SEMI) {
		return false;
	}
	// get the index (left or right) of the DelimGet side of the join
	idx_t delim_idx = join.children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET ||
	                          (join.children[0]->type == LogicalOperatorType::LOGICAL_FILTER &&
	                           join.children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET)
	                      ? 0
	                      : 1;
	// get the filter (if any)
	auto filter = (LogicalFilter *)(join.children[delim_idx]->type == LogicalOperatorType::LOGICAL_FILTER
	                                    ? join.children[delim_idx].get()
	                                    : nullptr);
	auto &delim_get =
	    (LogicalDelimGet &)*(filter != nullptr ? join.children[delim_idx]->children[0] : join.children[delim_idx]);
	if (join.conditions.size() != delim_get.chunk_types.size()) {
		// joining with DelimGet adds new information
		return false;
	}
	// check if joining with the DelimGet is redundant, and collect relevant column information
	vector<Expression *> nulls_are_not_equal_exprs;
	for (auto &cond : join.conditions) {
		if (cond.comparison != ExpressionType::COMPARE_EQUAL) {
			// non-equality join condition
			return false;
		}
		auto delim_side = delim_idx == 0 ? cond.left.get() : cond.right.get();
		auto other_side = delim_idx == 0 ? cond.right.get() : cond.left.get();
		if (delim_side->type != ExpressionType::BOUND_COLUMN_REF) {
			// non-colref e.g. expression -(4, 1) in 4-i=j where i is from DelimGet
			// FIXME: might be possible to also eliminate these
			return false;
		}
		updater.expr_map[delim_side] = other_side;
		if (!cond.null_values_are_equal) {
			nulls_are_not_equal_exprs.push_back(other_side);
		}
	}
	// removed DelimGet columns are assigned a new ColumnBinding by Projection/Aggregation, keep track here
	if (proj_or_agg.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		for (auto &cb : proj_or_agg.GetColumnBindings()) {
			updater.projection_map[cb] = true;
			for (auto &expr : nulls_are_not_equal_exprs) {
				if (proj_or_agg.expressions[cb.column_index]->Equals(expr)) {
					updater.projection_map[cb] = false;
					break;
				}
			}
		}
	} else {
		auto &agg = (LogicalAggregate &)proj_or_agg;
		for (auto &cb : agg.GetColumnBindings()) {
			updater.projection_map[cb] = true;
			for (auto &expr : nulls_are_not_equal_exprs) {
				if ((cb.table_index == agg.group_index && agg.groups[cb.column_index]->Equals(expr)) ||
				    (cb.table_index == agg.aggregate_index && agg.expressions[cb.column_index]->Equals(expr))) {
					updater.projection_map[cb] = false;
					break;
				}
			}
		}
	}
	// make a filter if needed
	if (!nulls_are_not_equal_exprs.empty() || filter != nullptr) {
		auto filter_op = make_unique<LogicalFilter>();
		if (!nulls_are_not_equal_exprs.empty()) {
			// add an IS NOT NULL filter that was implicitly in JoinCondition::null_values_are_equal
			auto is_not_null_expr =
			    make_unique<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			for (auto &expr : nulls_are_not_equal_exprs) {
				is_not_null_expr->children.push_back(expr->Copy());
			}
			filter_op->expressions.push_back(move(is_not_null_expr));
		}
		if (filter != nullptr) {
			for (auto &expr : filter->expressions) {
				filter_op->expressions.push_back(move(expr));
			}
		}
		filter_op->children.push_back(move(join.children[1 - delim_idx]));
		join.children[1 - delim_idx] = move(filter_op);
	}
	// temporarily save deleted operator so its expressions are still available
	updater.temp_ptr = move(proj_or_agg.children[0]);
	// replace the redundant join
	proj_or_agg.children[0] = move(join.children[1 - delim_idx]);
	return true;
}

} // namespace duckdb
