#include "ccv_nnc.h"
#include "ccv_nnc_easy.h"
#include "ccv_nnc_internal.h"
#include "ccv_internal.h"
#include "_ccv_nnc_graph.h"

ccv_nnc_graph_t* ccv_nnc_graph_new(void)
{
	ccv_nnc_graph_t* graph = (ccv_nnc_graph_t*)cccalloc(1, sizeof(ccv_nnc_graph_t));
	graph->exec_info = ccv_array_new(sizeof(ccv_nnc_graph_exec_info_t), 5, 0);
	return graph;
}

void ccv_nnc_graph_set_sources(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t* const sources, const int source_size)
{
	if (!graph->sources)
		graph->sources = ccv_array_new(sizeof(ccv_nnc_graph_exec_t), source_size, 0);
	else
		ccv_array_clear(graph->sources);
	int i;
	for (i = 0; i < source_size; i++)
		ccv_array_push(graph->sources, sources + i);
}

ccv_nnc_graph_exec_t* ccv_nnc_graph_sources(const ccv_nnc_graph_t* const graph)
{
	return graph->sources ? (ccv_nnc_graph_exec_t*)ccv_array_get(graph->sources, 0) : 0;
}

int ccv_nnc_graph_source_size(const ccv_nnc_graph_t* const graph)
{
	return graph->sources ? graph->sources->rnum : 0;
}

void ccv_nnc_graph_set_destinations(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t* const destinations, const int destination_size)
{
	if (!graph->destinations)
		graph->destinations = ccv_array_new(sizeof(ccv_nnc_graph_exec_t), destination_size, 0);
	else
		ccv_array_clear(graph->sources);
	int i;
	for (i = 0; i < destination_size; i++)
		ccv_array_push(graph->destinations, destinations + i);
}

ccv_nnc_graph_exec_t* ccv_nnc_graph_destinations(const ccv_nnc_graph_t* const graph)
{
	return graph->destinations ? (ccv_nnc_graph_exec_t*)ccv_array_get(graph->destinations, 0) : 0;
}

int ccv_nnc_graph_destination_size(const ccv_nnc_graph_t* const graph)
{
	return graph->destinations ? graph->destinations->rnum : 0;
}

void ccv_nnc_graph_exec_set_hint(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t exec, const ccv_nnc_hint_t hint)
{
	assert(exec.d < graph->exec_info->rnum);
	assert(exec.graph == graph);
	ccv_nnc_graph_exec_info_t* const exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, exec.d);
	exec_info->hint = hint;
}

static int _ccv_nnc_tensor_multiview_level_count(const ccv_nnc_tensor_multiview_t* const mv)
{
	assert(CCV_IS_TENSOR_MULTIVIEW(mv));
	const int count = mv->kind + mv->repeat;
	if (mv->tv)
		return 2; // For both current level, and tv level.
	int i, c = 0;
	for (i = 0; i < count; i++)
		c = ccv_max(c, _ccv_nnc_tensor_multiview_level_count(mv->data[i].ptr));
	return c + 1;
}

static ccv_nnc_graph_tensor_nest_t* _ccv_nnc_graph_tensor_nest_new(const ccv_nnc_tensor_multiview_t* const mv)
{
	const int level_count = _ccv_nnc_tensor_multiview_level_count(mv);
	ccv_nnc_graph_tensor_nest_t* tensor_nest = (ccv_nnc_graph_tensor_nest_t*)ccmalloc(sizeof(ccv_nnc_graph_tensor_nest_t) + sizeof(ccv_nnc_tensor_t*) * (level_count - 1));
	tensor_nest->broadcast_required = 0;
	tensor_nest->count = level_count;
	tensor_nest->index = 0;
	tensor_nest->tensors[0] = (ccv_nnc_tensor_t*)mv;
	return tensor_nest;
}

static void _ccv_nnc_graph_exec_rewind(ccv_nnc_graph_exec_info_t* const info)
{
	if (!info->tensor_nest_size)
		return;
	int i;
	// Rewind from tensor nests.
	for (i = 0; i < info->input_size; i++)
		if (info->tensor_nests[i])
			info->inputs[i] = info->tensor_nests[i]->tensors[0];
	const int d = info->input_size;
	for (i = 0; i < info->output_size; i++)
		if (info->tensor_nests[d + i])
			info->outputs[i] = info->tensor_nests[d + i]->tensors[0];
	const int dd = info->input_size + info->output_size;
	for (i = 0; i < info->broadcast_size; i++)
		if (info->tensor_nests[dd + i])
			info->broadcasts[i] = info->tensor_nests[dd + i]->tensors[0];
}

static void _ccv_nnc_graph_tensor_nest_free(ccv_nnc_graph_tensor_nest_t* const tensor_nest)
{
	ccfree(tensor_nest);
}

static void _ccv_nnc_graph_redo_tensor_nests(ccv_nnc_graph_exec_info_t* const info)
{
	int i;
	int has_nest = 0;
	for (i = 0; i < info->input_size && !has_nest; i++)
		has_nest = (info->inputs[i] && CCV_IS_TENSOR_MULTIVIEW(info->inputs[i]));
	for (i = 0; i < info->output_size && !has_nest; i++)
		has_nest = (info->outputs[i] && CCV_IS_TENSOR_MULTIVIEW(info->outputs[i]));
	for (i = 0; i < info->broadcast_size && !has_nest; i++)
		has_nest = (info->broadcasts[i] && CCV_IS_TENSOR_MULTIVIEW(info->broadcasts[i]));
	if (has_nest)
	{
		const int tensor_nest_size = info->input_size + info->output_size + info->broadcast_size;
		if (info->tensor_nests)
		{
			info->tensor_nests = (ccv_nnc_graph_tensor_nest_t**)ccrealloc(info->tensor_nests, sizeof(ccv_nnc_graph_tensor_nest_t*) * tensor_nest_size);
			for (i = info->tensor_nest_size; i < tensor_nest_size; i++)
				info->tensor_nests[i] = 0;
		} else
			info->tensor_nests = (ccv_nnc_graph_tensor_nest_t**)cccalloc(tensor_nest_size, sizeof(ccv_nnc_graph_tensor_nest_t*));
		info->tensor_nest_size = tensor_nest_size;
		for (i = 0; i < info->input_size; i++)
			if (CCV_IS_TENSOR_MULTIVIEW(info->inputs[i]))
			{
				if (!info->tensor_nests[i] || info->inputs[i] != info->tensor_nests[i]->tensors[0])
				{
					if (info->tensor_nests[i])
						_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[i]);
					info->tensor_nests[i] = _ccv_nnc_graph_tensor_nest_new((ccv_nnc_tensor_multiview_t*)info->inputs[i]);
				}
			} else {
				if (info->tensor_nests[i])
					_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[i]);
				info->tensor_nests[i] = 0;
			}
		const int d = info->input_size;
		for (i = 0; i < info->output_size; i++)
			if (CCV_IS_TENSOR_MULTIVIEW(info->outputs[i]))
			{
				if (!info->tensor_nests[d + i] || info->outputs[i] != info->tensor_nests[d + i]->tensors[0])
				{
					if (info->tensor_nests[d + i])
						_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[d + i]);
					info->tensor_nests[d + i] = _ccv_nnc_graph_tensor_nest_new((ccv_nnc_tensor_multiview_t*)info->outputs[i]);
				}
			} else {
				if (info->tensor_nests[d + i])
					_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[d + i]);
				info->tensor_nests[d + i] = 0;
			}
		const int dd = info->input_size + info->output_size;
		for (i = 0; i < info->broadcast_size; i++)
			if (CCV_IS_TENSOR_MULTIVIEW(info->broadcasts[i]))
			{
				if (!info->tensor_nests[dd + i] || info->broadcasts[dd + i] != info->tensor_nests[i]->tensors[0])
				{
					if (info->tensor_nests[dd + i])
						_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[dd + i]);
					info->tensor_nests[dd + i] = _ccv_nnc_graph_tensor_nest_new((ccv_nnc_tensor_multiview_t*)info->broadcasts[i]);
				}
			} else {
				if (info->tensor_nests[dd + i])
					_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[dd + i]);
				info->tensor_nests[i] = 0;
			}
	} else {
		for (i = 0; i < info->tensor_nest_size; i++)
			if (info->tensor_nests[i])
				_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[i]);
		ccfree(info->tensor_nests);
		info->tensor_nest_size = 0;
		info->tensor_nests = 0;
	}
}

static void _ccv_nnc_graph_deregister_tensor_nests(ccv_nnc_graph_t* graph, const ccv_nnc_graph_exec_t exec)
{
	ccv_nnc_graph_t* p = graph;
	do {
		int i;
		// Remove from the array.
		if (p->nest_execs)
			for (i = 0; i < p->nest_execs->rnum; i++)
			{
				ccv_nnc_graph_exec_t* const nest_exec = (ccv_nnc_graph_exec_t*)ccv_array_get(p->nest_execs, i);
				if (nest_exec->d == exec.d && nest_exec->graph == graph)
				{
					--p->nest_execs->rnum;
					if (i < p->nest_execs->rnum)
						memcpy(nest_exec, nest_exec + 1, sizeof(ccv_nnc_graph_exec_t) * (p->nest_execs->rnum - i));
					break;
				}
			}
		p = p->p;
	} while (p);
}

static void _ccv_nnc_graph_register_tensor_nests(ccv_nnc_graph_t* graph, const ccv_nnc_graph_exec_t exec, ccv_nnc_graph_exec_info_t* const info)
{
	assert(info->tensor_nest_size > 0);
	ccv_nnc_graph_t* p = graph;
	do {
		if (!p->nest_execs)
		{
			p->nest_execs = ccv_array_new(sizeof(ccv_nnc_graph_exec_t), 0, 0);
			ccv_array_push(p->nest_execs, &exec);
		} else {
			int i;
			int has_nest_exec = 0;
			for (i = 0; !has_nest_exec && i < p->nest_execs->rnum; i++)
			{
				ccv_nnc_graph_exec_t* nest_exec = (ccv_nnc_graph_exec_t*)ccv_array_get(p->nest_execs, i);
				has_nest_exec = (nest_exec->d == exec.d && nest_exec->graph == exec.graph);
			}
			if (!has_nest_exec)
				ccv_array_push(p->nest_execs, &exec);
		}
		p = p->p;
	} while (p);
}

void ccv_nnc_graph_exec_set_io(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t exec, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size)
{
	assert(exec.d < graph->exec_info->rnum);
	assert(exec.graph == graph);
	ccv_nnc_graph_exec_info_t* const info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, exec.d);
	// De-register from the graph if it contains multiview tensors.
	if (info->tensor_nest_size)
		_ccv_nnc_graph_deregister_tensor_nests(graph, exec);
	// In case it is already executed, rewind.
	_ccv_nnc_graph_exec_rewind(info);
	if (input_size == 0 && output_size == 0)
	{
		if (info->input_size > 0 || info->output_size > 0)
			ccfree(info->inputs);
		info->inputs = 0;
		info->outputs = 0;
		info->input_size = 0;
		info->output_size = 0;
		_ccv_nnc_graph_redo_tensor_nests(info);
		if (info->tensor_nest_size)
			_ccv_nnc_graph_register_tensor_nests(graph, exec, info);
		return;
	}
	if (info->inputs)
		info->inputs = (ccv_nnc_tensor_t**)ccrealloc(info->inputs, sizeof(ccv_nnc_tensor_t*) * (input_size + output_size));
	else
		info->inputs = (ccv_nnc_tensor_t**)ccmalloc(sizeof(ccv_nnc_tensor_t*) * (input_size + output_size));
	info->outputs = info->inputs + input_size;
	if (inputs)
		memcpy(info->inputs, inputs, sizeof(ccv_nnc_tensor_t*) * input_size);
	if (outputs)
		memcpy(info->outputs, outputs, sizeof(ccv_nnc_tensor_t*) * output_size);
	info->input_size = input_size;
	info->output_size = output_size;
	_ccv_nnc_graph_redo_tensor_nests(info);
	// Register again if the tensor nests exist.
	if (info->tensor_nest_size)
		_ccv_nnc_graph_register_tensor_nests(graph, exec, info);
}

void ccv_nnc_graph_exec_set_broadcast(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t exec, ccv_nnc_tensor_t* const* const broadcasts, const int broadcast_size)
{
	assert(exec.d < graph->exec_info->rnum);
	assert(exec.graph == graph);
	ccv_nnc_graph_exec_info_t* const info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, exec.d);
	// De-register from the graph if it contains multiview tensors.
	if (info->tensor_nest_size)
		_ccv_nnc_graph_deregister_tensor_nests(graph, exec);
	// In case it is already executed, rewind.
	_ccv_nnc_graph_exec_rewind(info);
	if (broadcast_size == 0)
	{
		if (info->broadcast_size > 0)
			ccfree(info->broadcasts);
		info->broadcasts = 0;
		info->broadcast_size = 0;
		return;
	}
	if (info->broadcasts)
		info->broadcasts = (ccv_nnc_tensor_t**)ccrealloc(info->broadcasts, sizeof(ccv_nnc_tensor_t*) * broadcast_size);
	else
		info->broadcasts = (ccv_nnc_tensor_t**)ccmalloc(sizeof(ccv_nnc_tensor_t*) * broadcast_size);
	if (broadcasts)
		memcpy(info->broadcasts, broadcasts, sizeof(ccv_nnc_tensor_t*) * broadcast_size);
	info->broadcast_size = broadcast_size;
	_ccv_nnc_graph_redo_tensor_nests(info);
	// Register again if the tensor nests exist.
	if (info->tensor_nest_size)
		_ccv_nnc_graph_register_tensor_nests(graph, exec, info);
}

ccv_nnc_graph_exec_t ccv_nnc_graph_exec_new(ccv_nnc_graph_t* const graph, const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size)
{
	int d = graph->exec_info->rnum;
	ccv_nnc_graph_exec_info_t info = {
		.cmd = cmd,
		.hint = hint,
		.input_size = input_size,
		.output_size = output_size,
	};
	assert(inputs || input_size == 0);
	assert(outputs || output_size == 0);
	if (input_size > 0 || output_size > 0)
	{
		info.inputs = (ccv_nnc_tensor_t**)ccmalloc(sizeof(ccv_nnc_tensor_t*) * (input_size + output_size));
		info.outputs = info.inputs + input_size;
		if (inputs)
			memcpy(info.inputs, inputs, sizeof(ccv_nnc_tensor_t*) * input_size);
		if (outputs)
			memcpy(info.outputs, outputs, sizeof(ccv_nnc_tensor_t*) * output_size);
		info.input_size = input_size;
		info.output_size = output_size;
	}
	ccv_nnc_graph_exec_t exec = {
		.d = d,
		.graph = graph,
	};
	_ccv_nnc_graph_redo_tensor_nests(&info);
	// Add itself to the graph's wraps array, this will help the run time when we run the graph and do unwrapping.
	if (info.tensor_nest_size)
		_ccv_nnc_graph_register_tensor_nests(graph, exec, &info);
	ccv_array_push(graph->exec_info, &info);
	return exec;
}

int ccv_nnc_graph_exec_concat(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t source, const ccv_nnc_graph_exec_t destination)
{
	assert(graph == source.graph);
	assert(graph == destination.graph);
	assert(source.d < graph->exec_info->rnum);
	assert(destination.d < graph->exec_info->rnum);
	ccv_nnc_graph_exec_info_t* src_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, source.d);
	if (src_info->outgoings == 0)
		src_info->outgoings = ccv_array_new(sizeof(int32_t), 1, 0);
	else {
		int i;
		// Check if this is already connected, if so, skip.
		for (i = 0; i < src_info->outgoings->rnum; i++)
			if (*(int*)ccv_array_get(src_info->outgoings, i) == destination.d)
				return -1;
	}
	ccv_array_push(src_info->outgoings, &destination.d);
	return 0;
}

int ccv_nnc_graph_exec_disjoin(ccv_nnc_graph_t* const graph, const ccv_nnc_graph_exec_t source, const ccv_nnc_graph_exec_t destination)
{
	assert(graph == source.graph);
	assert(graph == destination.graph);
	assert(source.d < graph->exec_info->rnum);
	assert(destination.d < graph->exec_info->rnum);
	ccv_nnc_graph_exec_info_t* src_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, source.d);
	if (!src_info->outgoings)
		return -1;
	int i, j = -1;
	// Check if this is already connected, if so, skip.
	for (i = 0; i < src_info->outgoings->rnum; i++)
		if (*(int*)ccv_array_get(src_info->outgoings, i) == destination.d)
		{
			j = i;
			break;
		}
	if (j < 0)
		return -1;
	if (j < src_info->outgoings->rnum - 1)
		*(int*)ccv_array_get(src_info->outgoings, j) = *(int*)ccv_array_get(src_info->outgoings, src_info->outgoings->rnum - 1);
	--src_info->outgoings->rnum;
	return 0;
}

static void _ccv_nnc_graph_dot_exec(const int index, const ccv_nnc_graph_exec_info_t* const exec_info, const int flags, FILE* out)
{
	if (flags == CCV_NNC_LONG_DOT_GRAPH)
		fputc('{', out);
	fprintf(out, "node%d", index);
	if (flags == CCV_NNC_LONG_DOT_GRAPH)
	{
		fputs("|Command: ", out);
		fputs(ccv_nnc_cmd_name(exec_info->cmd.cmd), out);
		fputc('}', out);
	}
}

static void _ccv_nnc_graph_dot_tensor(const int index, const ccv_nnc_tensor_t* const tensor, const int zone, const int flags, const int depth, FILE* out)
{
	// if it has an alias pointer, or, it is a long form.
	if (flags == CCV_NNC_LONG_DOT_GRAPH)
		fputc('{', out);
	int is_tensor_view = CCV_IS_TENSOR_VIEW(tensor);
	if (is_tensor_view)
		fprintf(out, "tensorview%d", index);
	else
		fprintf(out, "tensor%d", index);
	int i;
	for (i = 0; i < depth; i++) // Print subscription to denote depth.
		fputc('\'', out);
	if (flags == CCV_NNC_LONG_DOT_GRAPH)
	{
		fprintf(out, "|zone%d", zone);
		for (i = 0; i < depth; i++) // Print subscription to denote depth.
			fputc('\'', out);
		uintptr_t aptr = (uintptr_t)tensor->data.u8;
		const int* ainc = is_tensor_view ? ((ccv_nnc_tensor_view_t*)(tensor))->inc : tensor->info.dim;
		// For the last one, we don't extend to full ainc.
		size_t ainc_size = (ccv_nnc_dimension_count(ainc) - ainc[0] + tensor->info.dim[0]) * CCV_GET_DATA_TYPE_SIZE(tensor->type);
		// Print out the range as well.
		fprintf(out, "|{%#010x|%#010x}|%d", (uint32_t)aptr, (uint32_t)(aptr + ainc_size - 1), tensor->info.dim[0]);
		for (i = 1; i < CCV_NNC_MAX_DIM_ALLOC && tensor->info.dim[i]; i++)
			fprintf(out, "x%d", tensor->info.dim[i]);
		fputc('}', out);
	}
}

typedef struct {
	int index;
	int name;
	int zone;
	uintptr_t tensor_ref;
	uintptr_t start_ptr;
	uintptr_t end_ptr;
} ccv_nnc_tensor_dot_t;

typedef struct {
	ccv_nnc_tensor_dot_t* dots;
	int* remap;
	int* rename_zone;
	int* rename_index;
} ccv_nnc_tensor_dot_recovery_t;

// First sort by start_ptr, then sort by tensor ptr (so that we will have the same tensor sorted to one cluster).
#define less_than(i1, i2, aux) ((i1).start_ptr < (i2).start_ptr || ((i1).start_ptr == (i2).start_ptr && (i1).tensor_ref < (i2).tensor_ref))
static CCV_IMPLEMENT_QSORT(_ccv_nnc_tensor_dot_sort_by_ptr, ccv_nnc_tensor_dot_t, less_than)
#undef less_than

static int _ccv_nnc_graph_dot_tensor_multiview_count(const ccv_nnc_tensor_multiview_t* const mv)
{
	assert(CCV_IS_TENSOR_MULTIVIEW(mv));
	const int count = mv->kind + mv->repeat;
	if (mv->tv)
		return count;
	int i, c = 0;
	for (i = 0; i < count; i++)
		c += _ccv_nnc_graph_dot_tensor_multiview_count(mv->data[i].ptr);
	return c;
}

static void _ccv_nnc_graph_dot_tensor_multiview_tensor_dots(const ccv_nnc_tensor_multiview_t* const mv, ccv_nnc_tensor_dot_t* const tensor_dots, int* tensor_index)
{
	const int count = mv->kind + mv->repeat;
	int i;
	if (mv->tv)
		for (i = 0; i < count; i++)
		{
			tensor_dots[*tensor_index].name = *tensor_index;
			if (CCV_NNC_MULTIVIEW_K01(mv))
			{
				tensor_dots[*tensor_index].tensor_ref = (uintptr_t)mv->tv;
				tensor_dots[*tensor_index].start_ptr =  (uintptr_t)mv->tv->data.u8;
			} else {
				tensor_dots[*tensor_index].start_ptr =  (uintptr_t)mv->data[i].ptr;
				// Because tv's pointer will get updated, it is not correct in this case to have one tensor_ref.
				tensor_dots[*tensor_index].tensor_ref = tensor_dots[*tensor_index].start_ptr;
			}
			const size_t dim_size = ccv_nnc_dimension_count(mv->tv->info.dim) * CCV_GET_DATA_TYPE_SIZE(mv->tv->type);
			tensor_dots[*tensor_index].end_ptr = tensor_dots[*tensor_index].start_ptr + dim_size - 1;
			++(*tensor_index);
		}
	else
		for (i = 0; i < count; i++)
			_ccv_nnc_graph_dot_tensor_multiview_tensor_dots((ccv_nnc_tensor_multiview_t*)mv->data[i].ptr, tensor_dots, tensor_index);
}

static ccv_nnc_tensor_dot_recovery_t _ccv_nnc_graph_tensor_dot_recovery(const ccv_nnc_graph_t* const graph)
{
	int i, j;
	// Recover tensor relationships for all tensors referenced in the graph.
	// Most notably, we have to give these indexes, and find if they point to
	// the same memory region, and whether they overlap. These information
	// are lost since we converted from symbolic form to the execution form.
	// and here we do our best to recover because that is easier to understand
	// if we want to present the graph visually (also, we don't want to put this
	// information into the tensor or execution graph to avoid overhead, thus,
	// recovering is the best we can do).
	int tensor_count = 0;
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		ccv_nnc_graph_exec_info_t* exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		for (j = 0; j < exec_info->input_size; j++)
			if (exec_info->inputs[j])
				tensor_count += CCV_IS_TENSOR_MULTIVIEW(exec_info->inputs[j]) ? _ccv_nnc_graph_dot_tensor_multiview_count((ccv_nnc_tensor_multiview_t*)exec_info->inputs[j]) : 1;
		for (j = 0; j < exec_info->output_size; j++)
			if (exec_info->outputs[j])
				tensor_count += CCV_IS_TENSOR_MULTIVIEW(exec_info->outputs[j]) ? _ccv_nnc_graph_dot_tensor_multiview_count((ccv_nnc_tensor_multiview_t*)exec_info->outputs[j]) : 1;
	}
	ccv_nnc_tensor_dot_t* tensor_dots = tensor_count > 0 ? (ccv_nnc_tensor_dot_t*)ccmalloc(sizeof(ccv_nnc_tensor_dot_t) * tensor_count) : 0;
	int k = 0;
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		ccv_nnc_graph_exec_info_t* exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		for (j = 0; j < exec_info->input_size; j++)
		{
			ccv_nnc_tensor_t* tensor = exec_info->inputs[j];
			if (!tensor)
				continue;
			if (CCV_IS_TENSOR_MULTIVIEW(tensor))
				_ccv_nnc_graph_dot_tensor_multiview_tensor_dots((ccv_nnc_tensor_multiview_t*)tensor, tensor_dots, &k);
			else {
				tensor_dots[k].name = k;
				tensor_dots[k].tensor_ref = (uintptr_t)tensor;
				tensor_dots[k].start_ptr = (uintptr_t)tensor->data.u8;
				const int* inc = CCV_IS_TENSOR_VIEW(tensor) ? ((ccv_nnc_tensor_view_t*)tensor)->inc : tensor->info.dim;
				const size_t inc_size = (ccv_nnc_dimension_count(inc) - inc[0] + tensor->info.dim[0]) * CCV_GET_DATA_TYPE_SIZE(tensor->type);
				tensor_dots[k].end_ptr = tensor_dots[k].start_ptr + inc_size - 1;
				++k;
			}
		}
		for (j = 0; j < exec_info->output_size; j++)
		{
			ccv_nnc_tensor_t* tensor = exec_info->outputs[j];
			if (!tensor)
				continue;
			if (CCV_IS_TENSOR_MULTIVIEW(tensor))
				_ccv_nnc_graph_dot_tensor_multiview_tensor_dots((ccv_nnc_tensor_multiview_t*)tensor, tensor_dots, &k);
			else {
				tensor_dots[k].name = k;
				tensor_dots[k].tensor_ref = (uintptr_t)tensor;
				tensor_dots[k].start_ptr = (uintptr_t)tensor->data.u8;
				const int* inc = CCV_IS_TENSOR_VIEW(tensor) ? ((ccv_nnc_tensor_view_t*)tensor)->inc : tensor->info.dim;
				const size_t inc_size = (ccv_nnc_dimension_count(inc) - inc[0] + tensor->info.dim[0]) * CCV_GET_DATA_TYPE_SIZE(tensor->type);
				tensor_dots[k].end_ptr = tensor_dots[k].start_ptr + inc_size - 1;
				++k;
			}
		}
	}
	tensor_count = k; // We may over count, now shrink.
	// To group overlap memory into one zone, we sort it by start ptr first (secondary by the tensor pointer).
	_ccv_nnc_tensor_dot_sort_by_ptr(tensor_dots, tensor_count, 0);
	int index = 0, zone = 0;
	uintptr_t tensor_ref = tensor_count > 0 ? tensor_dots[0].tensor_ref : 0;
	uintptr_t end_ptr = tensor_count > 0 ? tensor_dots[0].end_ptr : 0;
	// Then, it is trivial, we go by end ptr. If the next start ptr is still within the end ptr (start ptr <= end ptr),
	// they are the same zone.
	for (i = 0; i < tensor_count; i++)
	{
		if (tensor_dots[i].tensor_ref != tensor_ref)
		{
			tensor_ref = tensor_dots[i].tensor_ref;
			++index;
		}
		if (tensor_dots[i].start_ptr > end_ptr)
		{
			end_ptr = ccv_max(end_ptr, tensor_dots[i].end_ptr);
			++zone;
		}
		tensor_dots[i].index = index;
		tensor_dots[i].zone = zone;
	}
	// We already have index and zone assigned, but the problem is that these are not very human interpretable (because
	// it follows the pointer from low to high, not the tensor creation order). The following code renamed both the index
	// and the zone so that it is much more understandable.
	const int index_count = index + 1;
	const int zone_count = zone + 1;
	int* remap = (int*)ccmalloc(sizeof(int) * (tensor_count + index_count + zone_count));
	int* rename_index = remap + tensor_count;
	int* rename_zone = rename_index + index_count;
	for (i = 0; i < tensor_count; i++)
		remap[tensor_dots[i].name] = i;
	for (i = 0; i < index_count; i++)
		rename_index[i] = -1;
	for (i = 0; i < zone_count; i++)
		rename_zone[i] = -1;
	index = 0;
	zone = 0;
	for (i = 0; i < tensor_count; i++)
	{
		ccv_nnc_tensor_dot_t* tensor_dot = tensor_dots + remap[i];
		if (rename_index[tensor_dot->index] == -1)
			rename_index[tensor_dot->index] = index++;
		if (rename_zone[tensor_dot->zone] == -1)
			rename_zone[tensor_dot->zone] = zone++;
	}
	ccv_nnc_tensor_dot_recovery_t recovery = {
		.dots = tensor_dots,
		.remap = remap,
		.rename_index = rename_index,
		.rename_zone = rename_zone,
	};
	return recovery;
}

static void _ccv_nnc_graph_tensor_dot_recovery_free(const ccv_nnc_tensor_dot_recovery_t recovery)
{
	ccfree(recovery.dots);
	ccfree(recovery.remap);
}

static void _ccv_nnc_graph_dot_tensor_multiview_one(const ccv_nnc_tensor_multiview_t* const mv, const ccv_nnc_tensor_dot_recovery_t recovery, const int depth, int* tensor_index, FILE* out)
{
	const int count = mv->kind + mv->repeat;
	int i, j;
	fputs("|{", out);
	if (mv->tv)
	{
		for (i = 0; i < count; i++)
		{
			fprintf(out, "{%d", i);
			if (mv->kind == CCV_NNC_MULTIVIEW_K0N || (mv->kind == CCV_NNC_MULTIVIEW_K1N && i > 0))
				fputc('*', out); // Denotes that we loop on this.
			const ccv_nnc_tensor_dot_t* const tensor_dot = recovery.dots + recovery.remap[*tensor_index];
			fprintf(out, "|zone%d", recovery.rename_zone[tensor_dot->zone]);
			for (j = 0; j < depth; j++)
				fputc('\'', out);
			uintptr_t aptr = (uintptr_t)(CCV_NNC_MULTIVIEW_K01(mv) ? mv->tv->data.u8 : mv->data[i].ptr);
			// For the last one, we don't extend to full ainc.
			size_t dim_size = ccv_nnc_dimension_count(mv->tv->info.dim) * CCV_GET_DATA_TYPE_SIZE(mv->tv->type);
			// Print out the range as well.
			fprintf(out, "|{%#010x|%#010x}", (uint32_t)aptr, (uint32_t)(aptr + dim_size - 1));
			++(*tensor_index);
			if (i == count - 1)
				fputc('}', out);
			else
				fputs("}|", out);
		}
	} else {
		for (i = 0; i < count; i++)
		{
			fprintf(out, "{%d", i);
			if (mv->kind == CCV_NNC_MULTIVIEW_K0N || (mv->kind == CCV_NNC_MULTIVIEW_K1N && i > 0))
				fputc('*', out); // Denotes that we loop on this.
			_ccv_nnc_graph_dot_tensor_multiview_one((ccv_nnc_tensor_multiview_t*)mv->data[i].ptr, recovery, depth, tensor_index, out);
			if (i == count - 1)
				fputc('}', out);
			else
				fputs("}|", out);
		}
	}
	fputc('}', out);
}

static void _ccv_nnc_graph_dot_tensor_multiview(const ccv_nnc_tensor_multiview_t* const mv, const ccv_nnc_tensor_dot_recovery_t recovery, const int flags, const int depth, int* tensor_index, FILE* out)
{
	// if it has an alias pointer, or, it is a long form.
	if (flags == CCV_NNC_LONG_DOT_GRAPH)
		fputc('{', out);
	const ccv_nnc_tensor_dot_t* const tensor_dot = recovery.dots + recovery.remap[*tensor_index];
	fprintf(out, "multiview%d", recovery.rename_index[tensor_dot->index]);
	int i;
	for (i = 0; i < depth; i++) // Print subscription to denote depth.
		fputc('\'', out);
	if (flags == CCV_NNC_LONG_DOT_GRAPH)
	{
		_ccv_nnc_graph_dot_tensor_multiview_one(mv, recovery, depth, tensor_index, out);
		const ccv_nnc_tensor_multiview_t* root = mv;
		while (!root->tv)
			root = (ccv_nnc_tensor_multiview_t*)(root->data[0].ptr);
		fprintf(out, "|%d", root->tv->info.dim[0]);
		for (i = 1; i < CCV_NNC_MAX_DIM_ALLOC && root->tv->info.dim[i]; i++)
			fprintf(out, "x%d", root->tv->info.dim[i]);
		fputc('}', out);
	} else
		*tensor_index += _ccv_nnc_graph_dot_tensor_multiview_count(mv);
}

static void _ccv_nnc_graph_dot_node(const ccv_nnc_graph_exec_info_t* const exec_info, const int exec_index, const ccv_nnc_tensor_dot_recovery_t recovery, const int flags, const int depth, FILE* out, int* const tensor_index)
{
	fprintf(out, "node%d [shape=record,label=\"", exec_index);
	_ccv_nnc_graph_dot_exec(exec_index, exec_info, flags, out);
	int i;
	int k = *tensor_index;
	if (exec_info->input_size > 0)
	{
		fputs("|{Input", out);
		for (i = 0; i < exec_info->input_size; i++)
			if (exec_info->inputs[i])
			{
				fputc('|', out);
				if (CCV_IS_TENSOR_MULTIVIEW(exec_info->inputs[i]))
					_ccv_nnc_graph_dot_tensor_multiview((ccv_nnc_tensor_multiview_t*)exec_info->inputs[i], recovery, flags, depth, &k, out);
				else {
					const ccv_nnc_tensor_dot_t* const tensor_dot = recovery.dots + recovery.remap[k];
					_ccv_nnc_graph_dot_tensor(recovery.rename_index[tensor_dot->index], exec_info->inputs[i], recovery.rename_zone[tensor_dot->zone], flags, depth, out);
					++k;
				}
			} else
				fputs("|-", out);
		fputc('}', out);
	}
	if (exec_info->output_size > 0)
	{
		fputs("|{Output", out);
		for (i = 0; i < exec_info->output_size; i++)
			if (exec_info->inputs[i])
			{
				fputc('|', out);
				if (CCV_IS_TENSOR_MULTIVIEW(exec_info->outputs[i]))
					_ccv_nnc_graph_dot_tensor_multiview((ccv_nnc_tensor_multiview_t*)exec_info->outputs[i], recovery, flags, depth, &k, out);
				else {
					const ccv_nnc_tensor_dot_t* const tensor_dot = recovery.dots + recovery.remap[k];
					_ccv_nnc_graph_dot_tensor(recovery.rename_index[tensor_dot->index], exec_info->outputs[i], recovery.rename_zone[tensor_dot->zone], flags, depth, out);
					++k;
				}
			} else
				fputs("|-", out);
		fputc('}', out);
	}
	fputs("\"];\n", out);
	*tensor_index = k;
}

static void _ccv_nnc_graph_dot_while_label(const ccv_nnc_graph_exec_info_t* const exec_info, const int exec_index, const ccv_nnc_tensor_dot_recovery_t recovery, const ccv_nnc_graph_t* const while_graph, const int flags, const int depth, FILE* out, int* tensor_index)
{
	int i;
	fprintf(out, "label=<<b>while%d</b>>;\n", exec_index);
	fputs("label [shape=record,label=\"{", out);
	int k = *tensor_index;
	if (exec_info->input_size > 0)
	{
		fputs("{Input|{", out);
		for (i = 0; i < exec_info->input_size; i++)
		{
			if (i > 0)
				fputc('|', out);
			if (exec_info->inputs[i])
			{
				if (CCV_IS_TENSOR_MULTIVIEW(exec_info->inputs[i]))
					_ccv_nnc_graph_dot_tensor_multiview((ccv_nnc_tensor_multiview_t*)exec_info->inputs[i], recovery, flags, depth, &k, out);
				else {
					const ccv_nnc_tensor_dot_t* const tensor_dot = recovery.dots + recovery.remap[k];
					_ccv_nnc_graph_dot_tensor(recovery.rename_index[tensor_dot->index], exec_info->inputs[i], recovery.rename_zone[tensor_dot->zone], flags, depth, out);
					++k;
				}
			} else
				fputc('-', out);
		}
		fputs("}}", out);
	}
	if (exec_info->output_size > 0)
	{
		if (exec_info->input_size > 0)
			fputs("|", out);
		fputs("{Output|{", out);
		for (i = 0; i < exec_info->output_size; i++)
		{
			if (i > 0)
				fputc('|', out);
			if (exec_info->outputs[i])
			{
				if (CCV_IS_TENSOR_MULTIVIEW(exec_info->outputs[i]))
					_ccv_nnc_graph_dot_tensor_multiview((ccv_nnc_tensor_multiview_t*)exec_info->outputs[i], recovery, flags, depth, &k, out);
				else {
					const ccv_nnc_tensor_dot_t* const tensor_dot = recovery.dots + recovery.remap[k];
					_ccv_nnc_graph_dot_tensor(recovery.rename_index[tensor_dot->index], exec_info->outputs[i], recovery.rename_zone[tensor_dot->zone], flags, depth, out);
					++k;
				}
			} else
				fputc('-', out);
		}
		fputs("}}", out);
	}
	fputs("}\"];\n", out);
	*tensor_index = k;
}

static void _ccv_nnc_graph_dot_sub_graph(const ccv_nnc_graph_exec_info_t* const exec_info, const ccv_nnc_tensor_dot_recovery_t p_recovery, const ccv_nnc_graph_t* const graph, const int flags, const int depth, FILE* out, int* tensor_index, int* exec_index)
{
	fprintf(out, "subgraph cluster%d {\nstyle=\"rounded\";\nnode%d [style=invisible];\n", *exec_index, *exec_index);
	// Output this node info within this subgraph.
	_ccv_nnc_graph_dot_while_label(exec_info, *exec_index, p_recovery, graph, flags, depth - 1 /* Label all references to its level above. */, out, tensor_index);
	++(*exec_index);
	ccv_nnc_tensor_dot_recovery_t recovery = _ccv_nnc_graph_tensor_dot_recovery(graph);
	int i, j;
	int k = 0;
	int* node_id = (int*)ccmalloc(sizeof(int) * graph->exec_info->rnum);
	// Output styles.
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		node_id[i] = *exec_index;
		ccv_nnc_graph_exec_info_t* exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		if (exec_info->graph_ref)
		{
			const ccv_nnc_graph_t* const while_graph = *(ccv_nnc_graph_t**)ccv_array_get(graph->sub_graphs, exec_info->graph_ref - 1);
			_ccv_nnc_graph_dot_sub_graph(exec_info, recovery, while_graph, flags, depth + 1, out, &k, exec_index);
		} else {
			_ccv_nnc_graph_dot_node(exec_info, *exec_index, recovery, flags, depth, out, &k);
			++(*exec_index);
		}
	}
	// Output connections.
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		ccv_nnc_graph_exec_info_t* exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		if (exec_info->outgoings)
			for (j = 0; j < exec_info->outgoings->rnum; j++)
			{
				const int outgoing_idx = *(int*)ccv_array_get(exec_info->outgoings, j);
				const ccv_nnc_graph_exec_info_t* const outgoing_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, outgoing_idx);
				// If both are sub-graphs, have both tail and head specified.
				if (exec_info->graph_ref && outgoing_info->graph_ref)
					fprintf(out, "node%d -> node%d [ltail=cluster%d,lhead=cluster%d];\n", node_id[i], node_id[outgoing_idx], node_id[i], node_id[outgoing_idx]);
				else if (exec_info->graph_ref && !outgoing_info->graph_ref)
					fprintf(out, "node%d -> node%d [ltail=cluster%d];\n", node_id[i], node_id[outgoing_idx], node_id[i]);
				else if (!exec_info->graph_ref && outgoing_info->graph_ref)
					fprintf(out, "node%d -> node%d [lhead=cluster%d];\n", node_id[i], node_id[outgoing_idx], node_id[outgoing_idx]);
				else
					fprintf(out, "node%d -> node%d;\n", node_id[i], node_id[outgoing_idx]);
			}
	}
	fputs("}\n", out);
	_ccv_nnc_graph_tensor_dot_recovery_free(recovery);
	ccfree(node_id);
}

void ccv_nnc_graph_dot(const ccv_nnc_graph_t* const graph, const int flags, FILE* out)
{
	fputs("digraph G {\ncompound=true;\n", out);
	ccv_nnc_tensor_dot_recovery_t recovery = _ccv_nnc_graph_tensor_dot_recovery(graph);
	int i, j;
	int k = 0, c = 0;
	int* node_id = (int*)ccmalloc(sizeof(int) * graph->exec_info->rnum);
	// Output styles.
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		node_id[i] = c;
		ccv_nnc_graph_exec_info_t* exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		if (exec_info->graph_ref)
		{
			const ccv_nnc_graph_t* const while_graph = *(ccv_nnc_graph_t**)ccv_array_get(graph->sub_graphs, exec_info->graph_ref - 1);
			_ccv_nnc_graph_dot_sub_graph(exec_info, recovery, while_graph, flags, 1, out, &k, &c);
		} else {
			_ccv_nnc_graph_dot_node(exec_info, c, recovery, flags, 0, out, &k);
			++c;
		}
	}
	// Output connections.
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		ccv_nnc_graph_exec_info_t* exec_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		if (exec_info->outgoings)
			for (j = 0; j < exec_info->outgoings->rnum; j++)
			{
				const int outgoing_idx = *(int*)ccv_array_get(exec_info->outgoings, j);
				const ccv_nnc_graph_exec_info_t* const outgoing_info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, outgoing_idx);
				// If both are sub-graphs, have both tail and head specified.
				if (exec_info->graph_ref && outgoing_info->graph_ref)
					fprintf(out, "node%d -> node%d [ltail=cluster%d,lhead=cluster%d];\n", node_id[i], node_id[outgoing_idx], node_id[i], node_id[outgoing_idx]);
				else if (exec_info->graph_ref && !outgoing_info->graph_ref)
					fprintf(out, "node%d -> node%d [ltail=cluster%d];\n", node_id[i], node_id[outgoing_idx], node_id[i]);
				else if (!exec_info->graph_ref && outgoing_info->graph_ref)
					fprintf(out, "node%d -> node%d [lhead=cluster%d];\n", node_id[i], node_id[outgoing_idx], node_id[outgoing_idx]);
				else
					fprintf(out, "node%d -> node%d;\n", node_id[i], node_id[outgoing_idx]);
			}
	}
	fputs("}\n", out);
	_ccv_nnc_graph_tensor_dot_recovery_free(recovery);
	ccfree(node_id);
}

void ccv_nnc_graph_autotune(ccv_nnc_graph_t* const graph, const size_t max_workspace_size, const int flags, const ccv_nnc_graph_exec_t* const sources, const int source_size, const ccv_nnc_graph_exec_t* const destinations, const int destination_size)
{
	// exec current node, for synchronous CPU execution, no stream unit.
#define visitor(node, idx, ...) \
	do { \
		node->cmd = ccv_nnc_cmd_autotune(node->cmd, max_workspace_size, node->hint, flags, node->inputs, node->input_size, node->outputs, node->output_size, 0); \
	} while (0)
	CCV_NNC_GRAPH_VISIT(graph, (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, 0), graph->exec_info->rnum, sources, source_size, destinations, destination_size, visitor);
#undef visitor
}

void ccv_nnc_graph_run(const ccv_nnc_graph_t* const graph, const int flags, const ccv_nnc_graph_exec_t* const sources, const int source_size, const ccv_nnc_graph_exec_t* const destinations, const int destination_size)
{
	// exec current node, for synchronous CPU execution, no stream unit.
	int i;
#define visitor(node, idx, d, ...) \
	do { \
		PRINT(CCV_CLI_VERBOSE, "%s [%d, %d]: [%d] -> [%d]\n", ccv_nnc_cmd_name(node->cmd.cmd), idx, d, node->input_size, node->output_size); \
		for (i = 0; i < node->input_size; i++) \
			PRINT(CCV_CLI_VERBOSE, "|-> %d. %p (%p)\n", i + 1, node->inputs[i], (node->inputs[i] ? node->inputs[i]->data.u8 : 0)); \
		for (i = 0; i < node->output_size; i++) \
			PRINT(CCV_CLI_VERBOSE, "|<- %d. %p (%p)\n", i + 1, node->outputs[i], (node->outputs[i] ? node->outputs[i]->data.u8 : 0)); \
		ccv_nnc_cmd_exec(node->cmd, node->hint, flags, node->inputs, node->input_size, node->outputs, node->output_size, 0); \
	} while (0)
	CCV_NNC_GRAPH_VISIT(graph, (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, 0), graph->exec_info->rnum, sources, source_size, destinations, destination_size, visitor);
#undef visitor
}

void ccv_nnc_graph_free(ccv_nnc_graph_t* const graph)
{
	int i, j;
	for (i = 0; i < graph->exec_info->rnum; i++)
	{
		ccv_nnc_graph_exec_info_t *info = (ccv_nnc_graph_exec_info_t*)ccv_array_get(graph->exec_info, i);
		ccv_array_t* outgoings = info->outgoings;
		if (outgoings)
			ccv_array_free(outgoings);
		// We allocate inputs & outputs in continuous fashion, therefore, only need to free the input array.
		if (info->inputs)
			ccfree(info->inputs);
		if (info->broadcasts)
			ccfree(info->broadcasts);
		if (info->tensor_nest_size)
		{
			for (j = 0; j < info->tensor_nest_size; j++)
				if (info->tensor_nests[j])
					_ccv_nnc_graph_tensor_nest_free(info->tensor_nests[j]);
			ccfree(info->tensor_nests);
		}
	}
	if (graph->breakpoints)
		ccfree(graph->breakpoints);
	if (graph->sources)
		ccv_array_free(graph->sources);
	if (graph->destinations)
		ccv_array_free(graph->destinations);
	if (graph->nest_execs)
		ccv_array_free(graph->nest_execs);
	if (graph->sub_graphs)
	{
		for (i = 0; i < graph->sub_graphs->rnum; i++)
			ccv_nnc_graph_free(*(ccv_nnc_graph_t**)ccv_array_get(graph->sub_graphs, i));
		ccv_array_free(graph->sub_graphs);
	}
	ccv_array_free(graph->exec_info);
	ccfree(graph);
}
