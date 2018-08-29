#include <ruby/ruby.h>
#include <ruby/debug.h>
#include <ruby/st.h>
#include <stdlib.h>

/*
digraph objects {
  node [ shape="record" ];

  trace_stats_t [
   label = "{
   <f0> trace_stats_t * |
   <f1> st_table * allocations
   }"
  ];

  allocations [ label = "{ stats-&gt;allocations | st_table * (numtable) }" ];
  file_table  [ label = "{ file_table | st_table * (strtable) }" ];
  line_table  [ label = "{ line_table | st_table * (numtable) }" ];
  class_name  [ label = "VALUE class_name" ];
  filename    [ label = "char * filename" ];
  line_number [ label = "unsigned long line_number" ];
  count       [ label = "unsigned long count" ];

  trace_stats_t:f1 -> allocations;
  allocations -> class_name  [ label = "key" ];
  allocations -> file_table  [ label = "value" ];
  file_table  -> filename    [ label = "key" ];
  file_table  -> line_table  [ label = "value" ];
  line_table  -> line_number [ label = "key" ];
  line_table  -> count       [ label = "value" ];
}
*/

typedef struct {
    VALUE *samples;
    size_t capa;
    size_t next_free;
    size_t prev_free;
    size_t record_count;
} sample_buffer_t;

typedef struct {
    st_table * allocations;
    size_t interval;
    size_t allocation_count;
    size_t overall_samples;
    sample_buffer_t * stack_samples;
    sample_buffer_t * lines_samples;
    VALUE newobj_hook;
} trace_stats_t;

typedef struct {
    sample_buffer_t * frames;
    sample_buffer_t * lines;
} compare_data_t;

static void
free_sample_buffer(sample_buffer_t *buffer)
{
    xfree(buffer->samples);
    xfree(buffer);
}

static sample_buffer_t *
alloc_sample_buffer(size_t size)
{
    sample_buffer_t * samples = xcalloc(sizeof(sample_buffer_t), 1);
    samples->samples = xcalloc(sizeof(VALUE), size);
    samples->capa = size;
    return samples;
}

static void
ensure_sample_buffer_capa(sample_buffer_t * buffer, size_t size)
{
    /* If we can't fit all the samples in the buffer, double the buffer size. */
    while (buffer->capa <= (buffer->next_free - 1) + (size + 2)) {
	buffer->capa *= 2;
	buffer->samples = xrealloc(buffer->samples, sizeof(VALUE) * buffer->capa);
    }
}

static void
dealloc(void *ptr)
{
    trace_stats_t * stats = (trace_stats_t *)ptr;
    sample_buffer_t * frames;
    sample_buffer_t * lines;

    frames = stats->stack_samples;
    lines = stats->lines_samples;

    if (frames && lines) {
	free_sample_buffer(frames);
	free_sample_buffer(lines);
    }
    xfree(stats);
}

static VALUE
make_frame_info(VALUE *frames, VALUE *lines)
{
    size_t count, i;
    VALUE rb_frames;

    count = *frames;
    frames++;
    lines++;

    rb_frames = rb_ary_new_capa(count);

    for(i = 0; i < count; i++, frames++, lines++) {
	rb_ary_push(rb_frames, rb_ary_new3(2, rb_obj_id(*frames), INT2NUM(*lines)));
    }

    return rb_frames;
}

static int
compare(void *ctx, size_t * l, size_t * r)
{
    compare_data_t *compare_data = (compare_data_t *)ctx;
    sample_buffer_t *stacks = compare_data->frames;
    sample_buffer_t *lines = compare_data->lines;

    size_t left_offset = *l;
    size_t right_offset = *r;

    size_t lstack = *(stacks->samples + left_offset);
    size_t rstack = *(stacks->samples + right_offset);

    if (lstack == rstack) {
	/* Compare the stack plus type info */
	int stack_cmp = memcmp(stacks->samples + left_offset, stacks->samples + right_offset, (lstack + 3) * sizeof(VALUE *));
	if (stack_cmp == 0) {
	    return memcmp(lines->samples + left_offset, lines->samples + right_offset, (lstack + 3) * sizeof(VALUE *));
	} else {
	    return stack_cmp;
	}
    } else {
	if (lstack < rstack) {
	    return -1;
	} else {
	    return 1;
	}
    }
}

static void
mark(void * ptr)
{
    trace_stats_t * stats = (trace_stats_t *)ptr;
    sample_buffer_t * stacks;

    stacks = stats->stack_samples;

    VALUE * frame = stacks->samples;

    while(frame < stacks->samples + stacks->next_free) {
	size_t stack_size;
	VALUE * head;

	stack_size = *frame;
	frame++; /* First element is the stack size */
	head = frame;

	for(; frame < (head + stack_size); frame++) {
	    rb_gc_mark(*frame);
	}
	frame++; /* Frame info */
	rb_gc_mark(*frame);
	frame++; /* Next Head */
    }
    rb_gc_mark(stats->newobj_hook);
}

static const rb_data_type_t trace_stats_type = {
    "ObjectSpace/AllocationSampler",
    {mark, dealloc, 0,},
    0, 0,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
    RUBY_TYPED_FREE_IMMEDIATELY,
#endif
};

static VALUE
user_class(VALUE klass, VALUE obj)
{
    if (RTEST(klass) && !(RB_TYPE_P(obj, T_IMEMO) || RB_TYPE_P(obj, T_NODE)) && BUILTIN_TYPE(klass) == T_CLASS) {
	return rb_class_path_cached(rb_class_real(klass));
    } else {
	return Qnil;
    }
}

#define BUF_SIZE 2048

static void
newobj(VALUE tpval, void *ptr)
{
    trace_stats_t * stats = (trace_stats_t *)ptr;

    if (!(stats->allocation_count % stats->interval)) {
	rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
	VALUE obj = rb_tracearg_object(tparg);
	VALUE klass = RBASIC_CLASS(obj);
	VALUE uc = user_class(klass, obj);

	if (!NIL_P(uc)) {
	    unsigned long count;
	    VALUE frames_buffer[BUF_SIZE];
	    int lines_buffer[BUF_SIZE];
	    int num;

	    VALUE path = rb_tracearg_path(tparg);
	    VALUE line = rb_tracearg_lineno(tparg);

	    if (RTEST(path)) {
		sample_buffer_t * stack_samples;
		sample_buffer_t * lines_samples;

		int num = rb_profile_frames(0, sizeof(frames_buffer) / sizeof(VALUE), frames_buffer, lines_buffer);
		if (!stats->stack_samples) {
		    stats->stack_samples = alloc_sample_buffer(num * 100);
		    stats->lines_samples = alloc_sample_buffer(num * 100);
		}
		stack_samples = stats->stack_samples;
		lines_samples = stats->lines_samples;

		ensure_sample_buffer_capa(stack_samples, num + 2);
		ensure_sample_buffer_capa(lines_samples, num + 2);

		stack_samples->prev_free = stack_samples->next_free;
		lines_samples->prev_free = lines_samples->next_free;

		stack_samples->samples[stack_samples->next_free] = (VALUE)num;
		lines_samples->samples[lines_samples->next_free] = (VALUE)num;

		memcpy(stack_samples->samples + stack_samples->next_free + 1, frames_buffer, num * sizeof(VALUE *));
		memcpy(lines_samples->samples + lines_samples->next_free + 1, lines_buffer, num * sizeof(int));

		stack_samples->samples[stack_samples->next_free + num + 2] = uc;
		stack_samples->next_free += (num + 3);
		lines_samples->next_free += (num + 3);

		stack_samples->record_count++;
		lines_samples->record_count++;

		stats->overall_samples++;
	    }
	}
    }
    stats->allocation_count++;
}

static VALUE
allocate(VALUE klass)
{
    trace_stats_t * stats;
    stats = xcalloc(sizeof(trace_stats_t), 1);
    stats->allocations = st_init_numtable();;
    stats->interval = 1;
    stats->newobj_hook = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_NEWOBJ, newobj, stats);

    return TypedData_Wrap_Struct(klass, &trace_stats_type, stats);
}

VALUE rb_cAllocationSampler;

static VALUE
enable(VALUE self)
{
    trace_stats_t * stats;
    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);
    rb_tracepoint_enable(stats->newobj_hook);
    return Qnil;
}

static VALUE
disable(VALUE self)
{
    trace_stats_t * stats;
    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);
    rb_tracepoint_disable(stats->newobj_hook);
    return Qnil;
}

static int
sort_frames(const void *left, const void *right)
{
    const VALUE *vleft = (const VALUE *)left;
    const VALUE *vright = (const VALUE *)right;
    /* Sort so that 0 is always at the right */
    if (*vleft == *vright) {
	return 0;
    } else {
	if (*vleft == 0) {
	    return 1;
	} else if (*vright == 0) {
	    return -1;
	}
    }
    return *vleft - *vright;
}

static VALUE
frames(VALUE self)
{
    trace_stats_t * stats;
    sample_buffer_t * frame_buffer;
    VALUE frames;
    VALUE * samples;
    VALUE *head;

    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);

    frame_buffer = stats->stack_samples;

    samples = xcalloc(sizeof(VALUE), frame_buffer->capa);
    memcpy(samples, frame_buffer->samples, frame_buffer->capa * sizeof(VALUE));

    /* Clear anything that's not a frame */
    for(head = samples; head < (samples + (frame_buffer->next_free - 1)); head++) {
	size_t frame_count;
	frame_count = *head;

	*head = 0;
	head++;              /* Skip the count */
	head += frame_count; /* Skip the stack */
	head++;              /* Skip the type */
	*head = 0;
    }

    qsort(samples, frame_buffer->capa, sizeof(VALUE), sort_frames);

    frames = rb_hash_new();

    for(head = samples; head < (samples + frame_buffer->capa); ) {
	if (*head == 0)
	    break;

	VALUE file;

	file = rb_profile_frame_absolute_path(*head);
	if (NIL_P(file))
	    file = rb_profile_frame_path(*head);

	rb_hash_aset(frames, rb_obj_id(*head),
		rb_ary_new3(2, rb_profile_frame_full_label(*head),
		               file));

	/* Skip duplicates */
	VALUE *cmp;
	for (cmp = head + 1; cmp < (samples + frame_buffer->capa); cmp++) {
	    if (*cmp != *head) {
		break;
	    }
	}
	head = cmp;
    }

    xfree(samples);

    return frames;
}

static VALUE
samples(VALUE self)
{
    trace_stats_t * stats;
    sample_buffer_t * frames;
    sample_buffer_t * lines;
    size_t *record_offsets;
    VALUE result = Qnil;

    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);

    frames = stats->stack_samples;
    lines = stats->lines_samples;

    if (frames && lines) {
	size_t i, j;
	size_t * head;
	VALUE * frame = frames->samples;
	compare_data_t compare_ctx;
	compare_ctx.frames = frames;
	compare_ctx.lines = lines;

	record_offsets = xcalloc(sizeof(size_t), frames->record_count);
	head = record_offsets;

	i = 0;
	while(frame < frames->samples + frames->next_free) {
	    *head = i;             /* Store the frame start offset */
	    head++;                /* Move to the next entry in record_offsets */
	    i     += (*frame + 3); /* Increase the offset */
	    frame += (*frame + 3); /* Move to the next frame */
	}

	qsort_r(record_offsets, frames->record_count, sizeof(size_t), &compare_ctx, compare);

	VALUE unique_frames = rb_ary_new();

	for(i = 0; i < frames->record_count; ) {
	    size_t current = record_offsets[i];
	    size_t count = 0;

	    /* Count any duplicate stacks ahead of us in the array */
	    for (j = i+1; j < frames->record_count; j++) {
		size_t next = record_offsets[j];
		int same = compare(&compare_ctx, &current, &next);

		if (same == 0) {
		    count++;
		} else {
		    break;
		}
	    }

	    i = j;

	    size_t stack_size = *(frames->samples + current);

	    VALUE type = *(frames->samples + current + stack_size + 2);

	    rb_ary_push(unique_frames,
		    rb_ary_new3(3,
			type,
			INT2NUM(count + 1),
			make_frame_info(frames->samples + current, lines->samples + current)));

	}

	xfree(record_offsets);

	result = unique_frames;
    }

    return result;
}

static VALUE
initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE opts;
    trace_stats_t * stats;

    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);
    rb_scan_args(argc, argv, ":", &opts);
    if (!NIL_P(opts)) {
	ID ids[2];
	VALUE args[2];
	ids[0] = rb_intern("interval");
	rb_get_kwargs(opts, ids, 0, 1, args);

	if (args[0] != Qundef) {
	    stats->interval = NUM2INT(args[0]);
	}
    }

    return self;
}

static VALUE
interval(VALUE self)
{
    trace_stats_t * stats;
    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);
    return INT2NUM(stats->interval);
}

static VALUE
allocation_count(VALUE self)
{
    trace_stats_t * stats;
    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);
    return INT2NUM(stats->allocation_count);
}

static VALUE
overall_samples(VALUE self)
{
    trace_stats_t * stats;
    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);
    return INT2NUM(stats->overall_samples);
}

void
Init_allocation_sampler(void)
{
    VALUE rb_mObjSpace = rb_const_get(rb_cObject, rb_intern("ObjectSpace"));
    rb_cAllocationSampler = rb_define_class_under(rb_mObjSpace, "AllocationSampler", rb_cObject);
    rb_define_alloc_func(rb_cAllocationSampler, allocate);
    rb_define_method(rb_cAllocationSampler, "initialize", initialize, -1);
    rb_define_method(rb_cAllocationSampler, "enable", enable, 0);
    rb_define_method(rb_cAllocationSampler, "disable", disable, 0);
    rb_define_method(rb_cAllocationSampler, "frames", frames, 0);
    rb_define_method(rb_cAllocationSampler, "samples", samples, 0);
    rb_define_method(rb_cAllocationSampler, "interval", interval, 0);
    rb_define_method(rb_cAllocationSampler, "allocation_count", allocation_count, 0);
    rb_define_method(rb_cAllocationSampler, "overall_samples", overall_samples, 0);
}
