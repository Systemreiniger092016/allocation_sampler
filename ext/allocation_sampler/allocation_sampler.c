#include <ruby/ruby.h>
#include <ruby/debug.h>
#include <ruby/st.h>

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
    st_table * allocations;
    size_t interval;
    size_t allocation_count;
    size_t overall_samples;
    VALUE newobj_hook;
} trace_stats_t;

typedef struct {
    unsigned long count;
    st_table *frames;
} allocation_data_t;

typedef struct {
    unsigned long line_number;
    size_t path_length;
    char * path;
    VALUE * frames_buffer;
    int * lines_buffer;
    int frame_count;
    size_t overall_samples;
} update_args_t;

typedef struct {
    size_t total_samples;
    size_t caller_samples;
    size_t seen_at_sample_number;
    st_table *edges;
    st_table *lines;
} frame_data_t;

static allocation_data_t *
make_allocation_data(void)
{
    allocation_data_t * allocation;

    allocation = (allocation_data_t *)xcalloc(1, sizeof(allocation_data_t));
    allocation->frames = st_init_numtable();
    return allocation;
}

static frame_data_t *
sample_for(allocation_data_t * allocation, VALUE frame)
{
    st_data_t key = (st_data_t)frame, val = 0;
    frame_data_t *frame_data;

    if (st_lookup(allocation->frames, key, &val)) {
        frame_data = (frame_data_t *)val;
    } else {
        frame_data = xcalloc(1, sizeof(frame_data_t));
	frame_data->edges = st_init_numtable();
	frame_data->lines = st_init_numtable();

        val = (st_data_t)frame_data;
        st_insert(allocation->frames, key, val);
    }

    return frame_data;
}

static int
numtable_increment_callback(st_data_t *key, st_data_t *value, st_data_t arg, int existing)
{
    size_t *weight = (size_t *)value;
    size_t increment = (size_t)arg;

    if (existing)
	(*weight) += increment;
    else
	*weight = increment;

    return ST_CONTINUE;
}

void
st_numtable_increment(st_table *table, st_data_t key, size_t increment)
{
    st_update(table, key, numtable_increment_callback, (st_data_t)increment);
}

static int
increment_line_count(st_data_t *line, st_data_t *data, st_data_t arg, int exists)
{
    allocation_data_t * allocation_data;
    frame_data_t * frame_data;
    int i;
    update_args_t *args = (update_args_t *)arg;

    if (exists) {
	allocation_data = (allocation_data_t *)*data;
    } else {
	allocation_data = make_allocation_data();
	*data = (st_data_t)allocation_data;
    }

    VALUE prev_frame;

    for(i = 0; i < args->frame_count; i++) {
	int line = args->lines_buffer[i];
	VALUE frame = args->frames_buffer[i];
	frame_data_t * frame_data = sample_for(allocation_data, frame);

	/* Don't count the same frame in a stack twice */
	if (frame_data->seen_at_sample_number != args->overall_samples) {
	    frame_data->total_samples++;
	}
	frame_data->seen_at_sample_number = args->overall_samples;

	if (i == 0) {
	    frame_data->caller_samples++;
	} else {
	    st_numtable_increment(frame_data->edges, (st_data_t)prev_frame, 1);
	}

	if (line > 0) {
	    /* Lower half is when the frame is at the top of the stack,
	     * upper half is non-top level frames */
	    size_t half = (size_t)1<<(8*SIZEOF_SIZE_T/2);
	    size_t increment = i == 0 ? half + 1 : half;
	    st_numtable_increment(frame_data->lines, (st_data_t)line, increment);
	}
	prev_frame = frame;
    }

    allocation_data->count++;
    return ST_CONTINUE;
}

static int
update_file_path_table(st_data_t *k, st_data_t *v, st_data_t arg, int exists)
{
    st_table * line_table;
    char * filename;
    size_t strlen;
    update_args_t *args = (update_args_t *)arg;

    if (exists) {
	line_table = (st_table *)*v;
    } else {
	line_table = st_init_numtable();
	*v = (st_data_t)line_table;

	strlen = args->path_length;
	filename = xmalloc(strlen + 1);
	strncpy(filename, (char *)*k, strlen);
	filename[strlen] = 0;
	*k = (st_data_t)filename;
    }

    return st_update(line_table, args->line_number, increment_line_count, arg);
}

static int
update_class_name_table(st_data_t *k, st_data_t *v, st_data_t arg, int exists)
{
    st_table * file_table;
    update_args_t *args = (update_args_t *)arg;

    if (exists) {
	file_table = (st_table *)*v;
    } else {
	file_table = st_init_strtable();
	*v = (st_data_t)file_table;
    }

    return st_update(file_table, (st_data_t)args->path, update_file_path_table, arg);
}

static int
free_frame_data_i(st_data_t frame, st_data_t _frame_data, st_data_t ctx)
{
    frame_data_t * frame_data = (frame_data_t *)_frame_data;
    st_free_table(frame_data->edges);
    st_free_table(frame_data->lines);
    return ST_CONTINUE;
}

static int
free_allocation_data_i(st_data_t line_number, st_data_t allocation_data, st_data_t ctx)
{
    allocation_data_t * allocation = (allocation_data_t *)allocation_data;
    st_foreach((st_table *)allocation->frames, free_frame_data_i, ctx);
    st_free_table(allocation->frames);
    xfree(allocation);
    return ST_CONTINUE;
}

static int
free_line_tables_i(st_data_t path, st_data_t line_table, st_data_t ctx)
{
    /* line table only contains long => long, so nothing to free */
    st_foreach((st_table *)line_table, free_allocation_data_i, ctx);
    st_free_table((st_table *)line_table);
    xfree((char *)path);
    return ST_CONTINUE;
}

static int
free_path_tables_i(st_data_t classpath, st_data_t path_table, st_data_t ctx)
{
    st_foreach((st_table *)path_table, free_line_tables_i, ctx);
    st_free_table((st_table *)path_table);
    return ST_CONTINUE;
}

static void
dealloc(void *ptr)
{
    trace_stats_t * stats = (trace_stats_t *)ptr;
    st_foreach(stats->allocations, free_path_tables_i, (st_data_t)stats);
    st_free_table(stats->allocations);
    xfree(stats);
}

static int
mark_frames_table_i(st_data_t key, st_data_t value, void *data)
{
    rb_gc_mark((VALUE)key); /* Mark the frame */
    return ST_CONTINUE;
}

static int
mark_line_table_i(st_data_t key, st_data_t value, void *data)
{
    allocation_data_t * allocation = (allocation_data_t *)value;
    st_foreach(allocation->frames, mark_frames_table_i, (st_data_t)data);
    return ST_CONTINUE;
}

static int
mark_file_table_i(st_data_t key, st_data_t value, void *data)
{
    st_table * line_table = (st_table *)value;
    st_foreach(line_table, mark_line_table_i, (st_data_t)data);
    return ST_CONTINUE;
}

static int
mark_class_table_i(st_data_t key, st_data_t value, void *data)
{
    st_table * file_table = (st_table *)value;

    rb_gc_mark((VALUE)key);
    st_foreach(file_table, mark_file_table_i, (st_data_t)data);
    return ST_CONTINUE;
}

static void
mark(void * ptr)
{
    trace_stats_t * stats = (trace_stats_t *)ptr;
    rb_gc_mark(stats->newobj_hook);
    st_foreach(stats->allocations, mark_class_table_i, (st_data_t)stats);
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
	    allocation_data_t * allocation_data;
	    VALUE frames_buffer[BUF_SIZE];
	    int lines_buffer[BUF_SIZE];
	    int num;

	    VALUE path = rb_tracearg_path(tparg);
	    VALUE line = rb_tracearg_lineno(tparg);

	    if (RTEST(path)) {
		update_args_t args;

		stats->overall_samples++;
		args.overall_samples = stats->overall_samples;
		args.frame_count     = rb_profile_frames(0, sizeof(frames_buffer) / sizeof(VALUE), frames_buffer, lines_buffer);
		args.line_number     = NUM2ULL(line);
		args.path            = RSTRING_PTR(path);
		args.path_length     = RSTRING_LEN(path);
		args.frames_buffer   = frames_buffer;
		args.lines_buffer    = lines_buffer;
		st_update(stats->allocations, (st_data_t)uc, update_class_name_table, (st_data_t)&args);
	    }
	}
    }
    stats->allocation_count++;
}

static VALUE
allocate(VALUE klass)
{
    trace_stats_t * stats;
    stats = xmalloc(sizeof(trace_stats_t));
    stats->allocations = st_init_numtable();;
    stats->interval = 1;
    stats->allocation_count = 0;
    stats->overall_samples = 0;
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
insert_line_to_ruby_hash(st_data_t line, st_data_t value, void *data)
{
    VALUE rb_line_hash = (VALUE)data;
    allocation_data_t * allocation_data = (allocation_data_t *)value;
    rb_hash_aset(rb_line_hash, ULL2NUM((unsigned long)line), ULL2NUM(allocation_data->count));
    return ST_CONTINUE;
}

static int
insert_path_to_ruby_hash(st_data_t path, st_data_t linetable, void *data)
{
    VALUE rb_path_hash = (VALUE)data;
    VALUE line_hash = rb_hash_new();
    rb_hash_aset(rb_path_hash, rb_str_new2((char *)path), line_hash);
    st_foreach((st_table *)linetable, insert_line_to_ruby_hash, line_hash);
    return ST_CONTINUE;
}

static int
insert_class_to_ruby_hash(st_data_t classname, st_data_t pathtable, void *data)
{
    VALUE rb_hash = (VALUE)data;
    VALUE path_hash = rb_hash_new();
    rb_hash_aset(rb_hash, (VALUE)classname, path_hash);
    st_foreach((st_table *)pathtable, insert_path_to_ruby_hash, path_hash);
    return ST_CONTINUE;
}

static VALUE
result(VALUE self)
{
    trace_stats_t * stats;
    VALUE result;

    TypedData_Get_Struct(self, trace_stats_t, &trace_stats_type, stats);

    result = rb_hash_new();
    st_foreach(stats->allocations, insert_class_to_ruby_hash, result);

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

void
Init_allocation_sampler(void)
{
    VALUE rb_mObjSpace = rb_const_get(rb_cObject, rb_intern("ObjectSpace"));
    rb_cAllocationSampler = rb_define_class_under(rb_mObjSpace, "AllocationSampler", rb_cObject);
    rb_define_alloc_func(rb_cAllocationSampler, allocate);
    rb_define_method(rb_cAllocationSampler, "initialize", initialize, -1);
    rb_define_method(rb_cAllocationSampler, "enable", enable, 0);
    rb_define_method(rb_cAllocationSampler, "disable", disable, 0);
    rb_define_method(rb_cAllocationSampler, "result", result, 0);
    rb_define_method(rb_cAllocationSampler, "interval", interval, 0);
    rb_define_method(rb_cAllocationSampler, "allocation_count", allocation_count, 0);
}
