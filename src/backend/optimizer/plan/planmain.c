/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * What's in a name, anyway?  The top-level entry point of the planner/
 * optimizer is over in planner.c, not here as you might think from the
 * file name.  But this is the main code for planning a basic join operation,
 * shorn of features like subselects, inheritance, aggregates, grouping,
 * and so on.  (Those are the things planner.c deals with.)
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planmain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"


/*
 * query_planner
 *	  Generate a path (that is, a simplified plan) for a basic query,
 *	  which may involve joins but not any fancier features.
 *
 * Since query_planner does not handle the toplevel processing (grouping,
 * sorting, etc) it cannot select the best path by itself.  Instead, it
 * returns the RelOptInfo for the top level of joining, and the caller
 * (grouping_planner) can choose among the surviving paths for the rel.
 *
 * root describes the query to plan
 * qp_callback is a function to compute query_pathkeys once it's safe to do so
 * qp_extra is optional extra data to pass to qp_callback
 *
 * Note: the PlannerInfo node also includes a query_pathkeys field, which
 * tells query_planner the sort order that is desired in the final output
 * plan.  This value is *not* available at call time, but is computed by
 * qp_callback once we have completed merging the query's equivalence classes.
 * (We cannot construct canonical pathkeys until that's done.)
 */
/*
 * query_planner
 * 生成一条路径（即简化方案）用于基本查询，
 * 可能涉及连接，但不涉及更复杂的功能。
 *
 * 由于query_planner不处理顶层处理（分组，
 * 排序等）它无法自行选择最佳路径。 相反，它
 * 返回连接最高层的RelOptInfo，以及呼叫者
 * （grouping_planner） 可以从存活路径中选择关系。
 *
 * 根描述了对计划的查询
 * qp_callback 是一个函数，用于在安全时计算query_pathkeys
 * qp_extra 是可选择的额外数据，可传递给qp_callback
 *
 * 注意：PlannerInfo 节点还包含一个query_pathkeys字段，其中
 * 告诉query_planner最终输出中所需的排序顺序
 * 计划。 该值在调用时*不*可用，但计算方式为
 * qp_callback 完成合并查询等价类后。
 * （在完成之前，我们无法构建规范路径。）
 */
RelOptInfo *
query_planner(PlannerInfo *root,
			  query_pathkeys_callback qp_callback, void *qp_extra)
{
	Query	   *parse = root->parse;
	List	   *joinlist;
	RelOptInfo *final_rel;

	/*
	 * Init planner lists to empty.
	 *
	 * NOTE: append_rel_list was set up by subquery_planner, so do not touch
	 * here.
	 */
	root->join_rel_list = NIL;
	root->join_rel_hash = NULL;
	root->join_rel_level = NULL;
	root->join_cur_level = 0;
	root->canon_pathkeys = NIL;
	root->left_join_clauses = NIL;
	root->right_join_clauses = NIL;
	root->full_join_clauses = NIL;
	root->join_info_list = NIL;
	root->placeholder_list = NIL;
	root->placeholder_array = NULL;
	root->placeholder_array_size = 0;
	root->fkey_list = NIL;
	root->initial_rels = NIL;

	/*
	 * Set up arrays for accessing base relations and AppendRelInfos.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * In the trivial case where the jointree is a single RTE_RESULT relation,
	 * bypass all the rest of this function and just make a RelOptInfo and its
	 * one access path.  This is worth optimizing because it applies for
	 * common cases like "SELECT expression" and "INSERT ... VALUES()".
	 */
/*
 * 在平凡情况下，jointree 是单RTE_RESULT关系，
 * 绕过所有其他功能，只创建一个 RelOptInfo 和
 * 一条通道。 这值得优化，因为它适用于
 * 常见情况如“SELECT 表达式”和“插入 ...VALUES（）“。
 */
	Assert(parse->jointree->fromlist != NIL);
	if (list_length(parse->jointree->fromlist) == 1)
	{
		Node	   *jtnode = (Node *) linitial(parse->jointree->fromlist);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = root->simple_rte_array[varno];

			Assert(rte != NULL);
			if (rte->rtekind == RTE_RESULT)
			{
				/* Make the RelOptInfo for it directly */
				final_rel = build_simple_rel(root, varno, NULL);

				/*
				 * If query allows parallelism in general, check whether the
				 * quals are parallel-restricted.  (We need not check
				 * final_rel->reltarget because it's empty at this point.
				 * Anything parallel-restricted in the query tlist will be
				 * dealt with later.)  This is normally pretty silly, because
				 * a Result-only plan would never be interesting to
				 * parallelize.  However, if debug_parallel_query is on, then
				 * we want to execute the Result in a parallel worker if
				 * possible, so we must do this.
				 */
/*
				 * 如果查询一般允许并行，请检查
				 * quals 是并行限制的。 （我们无需核实
				 * final_rel->reltarget，因为现在是空的。
				 * 查询列表中任何并行限制的内容将为
				 * 稍后处理。） 这通常很傻，因为
				 * 仅以结果为基础的计划永远不会有趣
				 * 平行化。 然而，如果debug_parallel_query开启，则
				 * 我们希望在并行工作者中执行结果，如果
				 * 可能，所以我们必须这么做。
				 */
				if (root->glob->parallelModeOK &&
					debug_parallel_query != DEBUG_PARALLEL_OFF)
					final_rel->consider_parallel =
						is_parallel_safe(root, parse->jointree->quals);

				/*
				 * The only path for it is a trivial Result path.  We cheat a
				 * bit here by using a GroupResultPath, because that way we
				 * can just jam the quals into it without preprocessing them.
				 * (But, if you hold your head at the right angle, a FROM-less
				 * SELECT is a kind of degenerate-grouping case, so it's not
				 * that much of a cheat.)
				 */
/*
 * 唯一的路径是一条平凡的结果路径。 我们作弊了
 * 通过使用GroupResultPath来比特，因为这样我们
 * 可以直接把资格证书塞进去，无需预处理。
 * （但是，如果你把头保持在正确的角度，一个无中场
 * SELECT 是一种简并分组情况，因此不是
 * 真是作弊。）
 */
				add_path(final_rel, (Path *)
						 create_group_result_path(root, final_rel,
												  final_rel->reltarget,
												  (List *) parse->jointree->quals));

				/* Select cheapest path (pretty easy in this case...) */
				set_cheapest(final_rel);

				/*
				 * We don't need to run generate_base_implied_equalities, but
				 * we do need to pretend that EC merging is complete.
				 */
/*
 * 我们不需要跑generate_base_implied_equalities，但
 * 我们确实需要假装EC合并已经完成。
 */
				root->ec_merging_done = true;

				/*
				 * We still are required to call qp_callback, in case it's
				 * something like "SELECT 2+2 ORDER BY 1".
				 */
				(*qp_callback) (root, qp_extra);

				return final_rel;
			}
		}
	}

	/*
	 * Construct RelOptInfo nodes for all base relations used in the query.
	 * Appendrel member relations ("other rels") will be added later.
	 *
	 * Note: the reason we find the baserels by searching the jointree, rather
	 * than scanning the rangetable, is that the rangetable may contain RTEs
	 * for rels not actively part of the query, for example views.  We don't
	 * want to make RelOptInfos for them.
	 */
/*
 * 为查询中使用的所有基关系构造 RelOptInfo 节点。
 * 附录成员关系（“其他关系”）将在后续添加。
 *
 * 注：我们找到baserel的原因，是通过搜索jointree，
 * 扫描距离表，意味着距离表可能包含RTE
 * 对于不活跃于查询中的 rels，例如视图。 我们没有
 * 想为他们制作RelOptInfos。
 */
	add_base_rels_to_query(root, (Node *) parse->jointree);

	/*
	 * Examine the targetlist and join tree, adding entries to baserel
	 * targetlists for all referenced Vars, and generating PlaceHolderInfo
	 * entries for all referenced PlaceHolderVars.  Restrict and join clauses
	 * are added to appropriate lists belonging to the mentioned relations. We
	 * also build EquivalenceClasses for provably equivalent expressions. The
	 * SpecialJoinInfo list is also built to hold information about join order
	 * restrictions.  Finally, we form a target joinlist for make_one_rel() to
	 * work from.
	 */
/*
 * 检查目标列表和连接树，向 baserel 添加条目
 * 所有引用变量的目标列表，并生成占位信息
 * 所有引用的占位变量条目。 限制与连接条款
 * 被添加到属于上述关系的适当列表中。我们
 * 还构建可证明等价表达式的等价类。该
 * SpecialJoinInfo列表还用于保存连接顺序的信息
 * 限制。 最后，我们为 make_one_rel（） 构建一个目标 joinlist 到
 * 工作来源。
 */
	build_base_rel_tlists(root, root->processed_tlist);

	find_placeholders_in_jointree(root);

	find_lateral_references(root);

	joinlist = deconstruct_jointree(root);

	/*
	 * Reconsider any postponed outer-join quals now that we have built up
	 * equivalence classes.  (This could result in further additions or
	 * mergings of classes.)
	 */
/*
 * 重新考虑任何推迟的外联盟资格，因为我们已经建立了
 * 等价类。 （这可能导致进一步的添加或
 * 课程合并。）
 */
	reconsider_outer_join_clauses(root);

	/*
	 * If we formed any equivalence classes, generate additional restriction
	 * clauses as appropriate.  (Implied join clauses are formed on-the-fly
	 * later.)
	 */

/*
	 * 如果我们形成任何等价类，则生成额外的限制
	 * 适当条款。 （隐含连接从句是即时形成的
	 * 稍后。）
	 */
	generate_base_implied_equalities(root);

	/*
	 * We have completed merging equivalence sets, so it's now possible to
	 * generate pathkeys in canonical form; so compute query_pathkeys and
	 * other pathkeys fields in PlannerInfo.
	 */
/*
 * 我们已完成等价集合并，因此现在可以
 * 以规范形式生成路径密钥;因此计算query_pathkeys 和
 * PlannerInfo 中的其他路径字段。
 */
	(*qp_callback) (root, qp_extra);

	/*
	 * Examine any "placeholder" expressions generated during subquery pullup.
	 * Make sure that the Vars they need are marked as needed at the relevant
	 * join level.  This must be done before join removal because it might
	 * cause Vars or placeholders to be needed above a join when they weren't
	 * so marked before.
	 */
/*
 * 检查子查询拉取过程中生成的任何“占位符”表达式。
 * 确保他们需要的变量在相关位置被标记为必需
 * 加入等级。 这必须在拆接之前完成，因为可能会
 * 使得在连接上方需要var或占位符，但实际上并非如此
 * 之前就这么标记了。
 */
	fix_placeholder_input_needed_levels(root);

	/*
	 * Remove any useless outer joins.  Ideally this would be done during
	 * jointree preprocessing, but the necessary information isn't available
	 * until we've built baserel data structures and classified qual clauses.
	 */
/*
 * 去除任何无用的外接缝。 理想情况下，这应该在
 * Jointree预处理，但所需信息尚未提供
 * 直到我们构建出baserel数据结构和分类定性子句。
 */
	joinlist = remove_useless_joins(root, joinlist);

	/*
	 * Also, reduce any semijoins with unique inner rels to plain inner joins.
	 * Likewise, this can't be done until now for lack of needed info.
	 */
	reduce_unique_semijoins(root);

	/*
	 * Now distribute "placeholders" to base rels as needed.  This has to be
	 * done after join removal because removal could change whether a
	 * placeholder is evaluable at a base rel.
	 */
/*
 * 现在根据需要将“占位符”分发到基础关系。 这一定是
 * 在移除连接后完成，因为移除可能会改变
 * 占位符在基础关系值。
 */
	add_placeholders_to_base_rels(root);

	/*
	 * Construct the lateral reference sets now that we have finalized
	 * PlaceHolderVar eval levels.
	 */
/*
 * 现在我们已经确定了，构建横向参考集
 * 占位变量评估等级。
 */
	create_lateral_join_info(root);

	/*
	 * Match foreign keys to equivalence classes and join quals.  This must be
	 * done after finalizing equivalence classes, and it's useful to wait till
	 * after join removal so that we can skip processing foreign keys
	 * involving removed relations.
	 */
/*
 * 将外键匹配到等价类并加入quals。 这一定是
 * 在确定等价类后完成，建议等到
 * 连接移除后，我们可以跳过处理外键
 * 涉及已移除的亲属。
 */
	match_foreign_keys_to_quals(root);

	/*
	 * Look for join OR clauses that we can extract single-relation
	 * restriction OR clauses from.
	 */
	extract_restriction_or_clauses(root);

	/*
	 * Now expand appendrels by adding "otherrels" for their children.  We
	 * delay this to the end so that we have as much information as possible
	 * available for each baserel, including all restriction clauses.  That
	 * let us prune away partitions that don't satisfy a restriction clause.
	 * Also note that some information such as lateral_relids is propagated
	 * from baserels to otherrels here, so we must have computed it already.
	 */
/*
 * 现在通过添加“otherrels”来扩展附录，代表他们的子女。 我们
 * 把这事推迟到最后，以便我们能获得尽可能多的信息
 * 适用于每个基底，包括所有限制条款。 那个
 * 让我们修剪那些不符合限制条款的分区。
 * 还要注意，一些信息如lateral_relids会传播
 * 从基面到其他子，所以我们应该已经计算过了。
 */
	add_other_rels_to_query(root);

	/*
	 * Distribute any UPDATE/DELETE/MERGE row identity variables to the target
	 * relations.  This can't be done till we've finished expansion of
	 * appendrels.
	 */
/*
 * 将任何 UPDATE/DELETE/MERGE 行身份变量分发给目标
 * 关系。 在我们完成 的扩展之前，这无法完成
 * 附录。
 */
	distribute_row_identity_vars(root);

	/*
	 * Ready to do the primary planning.
	 */
	final_rel = make_one_rel(root, joinlist);

	/* Check that we got at least one usable path */
	if (!final_rel || !final_rel->cheapest_total_path ||
		final_rel->cheapest_total_path->param_info != NULL)
		elog(ERROR, "failed to construct the join relation");

	return final_rel;
}
