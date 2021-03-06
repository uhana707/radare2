/* Copyright radare2 2014-2015 - Author: pancake */

#include <r_core.h>
static const char *mousemodes[] = { "canvas-y", "canvas-x", "node-y", "node-x", NULL };
static int mousemode = 0;
static int zoom = 100;

#define BORDER 3
#define BORDER_WIDTH 4
#define BORDER_HEIGHT 3
#define MARGIN_TEXT_X 2
#define MARGIN_TEXT_Y 2
#define HORIZONTAL_NODE_SPACING 12
#define VERTICAL_NODE_SPACING 4
#define MAX_NODE_WIDTH 18
#define INIT_HISTORY_CAPACITY 16
#define TITLE_LEN 128

#define history_push(stack, x) (r_stack_push (stack, (void *)(size_t)x))
#define history_pop(stack) ((RGraphNode *)r_stack_pop (stack))

#define hash_set(sdb,k,v) (sdb_num_set (sdb, sdb_fmt (0, "%"PFMT64u, (ut64)(size_t)k), (ut64)(size_t)v, 0))
#define hash_get(sdb,k) (sdb_num_get (sdb, sdb_fmt (0, "%"PFMT64u, (ut64)(size_t)k), NULL))
#define hash_get_rnode(sdb,k) ((RGraphNode *)(size_t)hash_get (sdb, k))

#define get_gn(iter) ((RGraphNode *)r_list_iter_get_data(iter))
#define get_anode(iter) ((ANode *)get_gn(iter)->data)

#define graph_foreach_anode(list, it, pos, anode) \
	if (list) for (it = list->head; it && (pos = it->data) && (pos) && (anode = (ANode *)pos->data); it = it->n)

struct layer_t {
	int n_nodes;
	RGraphNode **nodes;
	int position;
	int height;
};

typedef struct ascii_node {
	int x;
	int y;
	int w;
	int h;
	ut64 addr;
	int layer;
	int pos_in_layer;
	char *text;
	int is_dummy;
} ANode;

typedef struct ascii_graph {
	RCore *core;
	RConsCanvas *can;
	RAnalFunction *fcn;
	RGraph *graph;
	RListIter *curnode;

	int is_callgraph;
	int is_instep;
	int is_simple_mode;
	int is_small_nodes;

	RStack *history;
	ANode *update_seek_on;
	int need_reload_nodes;
	int force_update_seek;

	/* layout algorithm info */
	RList *back_edges;
	RList *long_edges;
	struct layer_t *layers;
	int n_layers;
} AGraph;

struct agraph_refresh_data {
	AGraph *g;
	int fs;
};

#define G(x,y) r_cons_canvas_gotoxy (g->can, x, y)
#define W(x) r_cons_canvas_write (g->can, x)
#define B(x,y,w,h) r_cons_canvas_box(g->can, x,y,w,h,NULL)
#define B1(x,y,w,h) r_cons_canvas_box(g->can, x,y,w,h,Color_BLUE)
#define B2(x,y,w,h) r_cons_canvas_box(g->can, x,y,w,h,Color_MAGENTA)
#define L(x,y,x2,y2) r_cons_canvas_line(g->can, x,y,x2,y2,0)
#define L1(x,y,x2,y2) r_cons_canvas_line(g->can, x,y,x2,y2,1)
#define L2(x,y,x2,y2) r_cons_canvas_line(g->can, x,y,x2,y2,2)
#define F(x,y,x2,y2,c) r_cons_canvas_fill(g->can, x,y,x2,y2,c,0)

static void update_node_dimension(RGraph *g, int is_small) {
	const RList *nodes = r_graph_get_nodes (g);
	RGraphNode *gn;
	RListIter *it;
	ANode *n;

	graph_foreach_anode (nodes, it, gn, n) {
		if (is_small) {
			n->w = n->h = 0;
		} else {
			n->w = r_str_bounds (n->text, &n->h);
			n->w += BORDER_WIDTH;
			n->h += BORDER_HEIGHT;
			n->w = R_MAX (MAX_NODE_WIDTH, n->w);
			/* scale node by zoom */
			n->w = (n->w * zoom) / 100;
			n->h = (n->h * zoom) / 100;
		}
	}
}

static void small_ANode_print(AGraph *g, ANode *n, int cur) {
	char title[TITLE_LEN];

	if (!G (n->x + 2, n->y - 1))
		return;
	if (cur) {
		W("[_@@_]");
		(void)G (-g->can->sx, -g->can->sy + 2);
		snprintf (title, sizeof (title) - 1,
				"0x%08"PFMT64x":", n->addr);
		W (title);
		(void)G (-g->can->sx, -g->can->sy + 3);
		W (n->text);
	} else {
		W("[____]");
	}
	return;
}

static void normal_ANode_print(AGraph *g, ANode *n, int cur) {
	char title[TITLE_LEN];
	char *text;
	int delta_x = 0;
	int delta_y = 0;
	int x, y;

#if SHOW_OUT_OF_SCREEN_NODES
	x = n->x + g->can->sx;
	y = n->y + n->h + g->can->sy;
	if (x < 0 || x > g->can->w)
		return;
	if (y < 0 || y > g->can->h)
		return;
#endif
	x = n->x + g->can->sx;
	y = n->y + g->can->sy;
	if (x < -MARGIN_TEXT_X)
		delta_x = -x - MARGIN_TEXT_X;
	if (x + n->w < -MARGIN_TEXT_X)
		return;
	if (y < -1)
		delta_y = -y - MARGIN_TEXT_Y;

	if (cur) {
		//F (n->x,n->y, n->w, n->h, '.');
		snprintf (title, sizeof (title)-1,
				"[0x%08"PFMT64x"]", n->addr);
	} else {
		snprintf (title, sizeof (title)-1,
				" 0x%08"PFMT64x" ", n->addr);
	}
	if (delta_x < strlen(title) && G(n->x + MARGIN_TEXT_X + delta_x, n->y + 1))
		W(title + delta_x);
	int center_x = 0;
	int center_y = 0;
	if (zoom>100) {
		center_x += ((zoom-100)/20);
		center_y += ((zoom-100)/30);
	}

	if (G(n->x + MARGIN_TEXT_X + delta_x + center_x,
			n->y + MARGIN_TEXT_Y + delta_y + center_y)) {
		text = r_str_crop (n->text,
			delta_x, delta_y,
			n->w - BORDER_WIDTH,
			n->h - BORDER_WIDTH + 1);
		if (text) {
			W (text);
			free (text);
		} else {
			W (n->text);
		}
	}

	// TODO: check if node is traced or not and hsow proper color
	// This info must be stored inside ANode* from RCore*
	if (cur) {
		B1 (n->x, n->y, n->w, n->h);
	} else {
		B (n->x, n->y, n->w, n->h);
	}
}

static int **get_crossing_matrix (const RGraph *g, struct layer_t layers[],
								  int maxlayer, int i, int *n_rows) {
	int len = layers[i].n_nodes;
	int **m;
	int j;

	m = R_NEWS0 (int *, len);
	if (!m)
		return NULL;

	for (j = 0; j < len; ++j) {
		m[j] = R_NEWS0 (int, len);
		if (!m[j])
			goto err_row;
	}

	/* calculate crossings between layer i and layer i-1 */
	/* consider the crossings generated by each pair of edges */
	if (i > 0) {
		int prev_len = layers[i - 1].n_nodes;
		for (j = 0; j < prev_len; ++j) {
			RGraphNode *gj = layers[i - 1].nodes[j];
			const RList *neigh = r_graph_get_neighbours (g, gj);
			RGraphNode *gk;
			RListIter *itk;

			r_list_foreach (neigh, itk, gk) {
				int s;
				for (s = 0; s < j; ++s) {
					RGraphNode *gs = layers[i - 1].nodes[s];
					const RList *neigh_s = r_graph_get_neighbours (g, gs);
					RGraphNode *gt;
					RListIter *itt;

					r_list_foreach (neigh_s, itt, gt) {
						ANode *ak, *at; /* k and t should be "indexes" on layer i */

						if (gt == gk) continue;
						ak = (ANode *)gk->data;
						at = (ANode *)gt->data;
						if (ak->layer != i || at->layer != i) {
							 eprintf("%llx or %llx are not on the right layer (%d)\n", ak->addr, at->addr, i);
							 eprintf("edge from %llx to %llx is wrong\n", ((ANode*)(gj->data))->addr, ak->addr);
							 eprintf("edge from %llx to %llx is wrong\n\n", ((ANode*)(gs->data))->addr, at->addr);

							 continue;
						}
						m[ak->pos_in_layer][at->pos_in_layer]++;
					}
				}
			}
		}
	}

	/* calculate crossings between layer i and layer i+1 */
	if (i < maxlayer - 1) {
		/* XXX: implement */
	}

	if (n_rows)
		*n_rows = len;
	return m;

err_row:
	for (i = 0; i < len; ++i) {
		if (m[i])
			free (m[i]);
	}
	free (m);
	return NULL;
}

static int layer_sweep (RGraph *g, struct layer_t layers[], int maxlayer, int i) {
	int **cross_matrix;
	RGraphNode *u, *v;
	ANode *au, *av;
	int n_rows, j, changed = R_FALSE;
	int len = layers[i].n_nodes;

	cross_matrix = get_crossing_matrix (g, layers, maxlayer, i, &n_rows);
	if (!cross_matrix) return R_FALSE;

	for (j = 0; j < len - 1; ++j) {
		int auidx, avidx;

		u = layers[i].nodes[j];
		v = layers[i].nodes[j + 1];
		au = (ANode *)u->data;
		av = (ANode *)v->data;
		auidx = au->pos_in_layer;
		avidx = av->pos_in_layer;

		if (cross_matrix[auidx][avidx] > cross_matrix[avidx][auidx]) {
			/* swap elements */
			layers[i].nodes[j] = v;
			layers[i].nodes[j + 1] = u;
			changed = R_TRUE;
		}
	}

	/* update position in the layer of each node. During the swap of some
	 * elements we didn't swap also the pos_in_layer because the cross_matrix
	 * is indexed by it, so do it now! */
	for (j = 0; j < layers[i].n_nodes; ++j) {
		ANode *n = (ANode *)layers[i].nodes[j]->data;
		n->pos_in_layer = j;
	}

	for (j = 0; j < n_rows; ++j)
		free (cross_matrix[j]);
	free (cross_matrix);
	return changed;
}

static void view_cyclic_edge (RGraphNode *from, RGraphNode *to, RGraphVisitor *vis) {
	AGraph *g = (AGraph *)vis->data;
	RGraphEdge *e = R_NEW (RGraphEdge);

	e->from = from;
	e->to = to;
	r_list_append (g->back_edges, e);
}

static int get_depth (Sdb *path, RGraphNode *n) {
	int res = 0;
	while ((n = hash_get_rnode (path, n)) != NULL) {
		res++;
	}
	return res;
}

static void set_layer (RGraphNode *from, RGraphNode *to, RGraphVisitor *vis) {
	Sdb *path = (Sdb *)vis->data;
	int bdepth, adepth;

	adepth = get_depth (path, from);
	bdepth = get_depth (path, to);

	if (adepth + 1 > bdepth)
		hash_set (path, to, from);
}

static void view_dummy (RGraphNode *from, RGraphNode *to, RGraphVisitor *vis) {
	ANode *a = (ANode *)from->data;
	ANode *b = (ANode *)to->data;
	RList *long_edges = (RList *)vis->data;

	if (R_ABS (a->layer - b->layer) > 1) {
		RGraphEdge *e = R_NEW (RGraphEdge);
		e->from = from;
		e->to = to;
		r_list_append (long_edges, e);
	}
}

/* find a set of edges that, removed, makes the graph acyclic */
/* invert the edges identified in the previous step */
static void remove_cycles (AGraph *g) {
	RGraphVisitor cyclic_vis = { 0 };
	RGraphEdge *e;
	RListIter *it;

	g->back_edges = r_list_new();
	cyclic_vis.back_edge = (RGraphEdgeCallback)view_cyclic_edge;
	cyclic_vis.data = g;
	r_graph_dfs (g->graph, &cyclic_vis);

	r_list_foreach (g->back_edges, it, e) {
		 r_graph_del_edge (g->graph, e->from, e->to);
		 r_graph_add_edge (g->graph, e->to, e->from);
	}
}

/* assign a layer to each node of the graph */
static void assign_layers (AGraph *g) {
	RGraphVisitor layer_vis = { 0 };
	Sdb *path_layers = sdb_new0 ();
	RGraphNode *gn;
	RListIter *it;
	ANode *n;

	layer_vis.data = path_layers;
	layer_vis.tree_edge = (RGraphEdgeCallback)set_layer;
	layer_vis.fcross_edge = (RGraphEdgeCallback)set_layer;
	r_graph_dfs (g->graph, &layer_vis);

	graph_foreach_anode (r_graph_get_nodes (g->graph), it, gn, n) {
		n->layer = get_depth (path_layers, gn);
	}

	sdb_free (path_layers);
}

/* add dummy nodes when there are edges that span multiple layers */
static void create_dummy_nodes (AGraph *g) {
	RGraphVisitor dummy_vis = { 0 };
	RListIter *it;
	RGraphEdge *e;

	g->long_edges = r_list_new ();
	dummy_vis.data = g->long_edges;
	dummy_vis.tree_edge = (RGraphEdgeCallback)view_dummy;
	dummy_vis.fcross_edge = (RGraphEdgeCallback)view_dummy;
	r_graph_dfs (g->graph, &dummy_vis);

	r_list_foreach (g->long_edges, it, e) {
		ANode *from = (ANode *)e->from->data;
		ANode *to = (ANode *)e->to->data;
		int diff_layer = R_ABS (from->layer - to->layer);
		RGraphNode *prev = e->from;
		int i;

		r_graph_del_edge (g->graph, e->from, e->to);
		for (i = 1; i < diff_layer; ++i) {
			ANode *n = R_NEW0 (ANode);
			n->layer = from->layer + i;
			n->pos_in_layer = -1;
			n->is_dummy = R_TRUE;
			RGraphNode *dummy = r_graph_add_node (g->graph, n);
			r_graph_add_edge (g->graph, prev, dummy);
			prev = dummy;
		}
		r_graph_add_edge (g->graph, prev, e->to);
	}
}

/* create layers and assign an initial ordering of the nodes into them */
static void create_layers (AGraph *g) {
	const RList *nodes = r_graph_get_nodes (g->graph);
	RGraphNode *gn;
	RListIter *it;
	ANode *n;
	int i;

	/* identify max layer */
	g->n_layers = 0;
	graph_foreach_anode (nodes, it, gn, n) {
		if (n->layer > g->n_layers)
			g->n_layers = n->layer;
	}

	/* create a starting ordering of nodes for each layer */
	g->n_layers++;
	g->layers = R_NEWS0 (struct layer_t, g->n_layers);

	graph_foreach_anode (nodes, it, gn, n)
		g->layers[n->layer].n_nodes++;

	for (i = 0; i < g->n_layers; ++i) {
		g->layers[i].nodes = R_NEWS (RGraphNode *, g->layers[i].n_nodes);
		g->layers[i].position = 0;
	}
	graph_foreach_anode (nodes, it, gn, n) {
		n->pos_in_layer = g->layers[n->layer].position;
		g->layers[n->layer].nodes[g->layers[n->layer].position++] = gn;
	}
}

/* layer-by-layer sweep */
/* it permutes each layer, trying to find the best ordering for each layer
 * to minimize the number of crossing edges */
static void minimize_crossings (AGraph *g) {
	int i, cross_changed;

	do {
		cross_changed = R_FALSE;

		for (i = 0; i < g->n_layers; ++i)
			cross_changed |= layer_sweep (g->graph, g->layers, g->n_layers, i);
	} while (cross_changed);
}

static void restore_original_edges (AGraph *g) {
	RListIter *it;
	RGraphEdge *e;

	r_list_foreach (g->long_edges, it, e) {
		r_graph_add_edge (g->graph, e->from, e->to);
	}

	r_list_foreach (g->back_edges, it, e) {
		r_graph_del_edge (g->graph, e->to, e->from);
		r_graph_add_edge (g->graph, e->from, e->to);
	}
}

static void remove_dummy_nodes (AGraph *g) {
	RGraphNode *gn;
	RListIter *it;
	ANode *n;
	RList *toremove = r_list_new ();

	graph_foreach_anode (r_graph_get_nodes (g->graph), it, gn, n) {
		if (n->is_dummy) {
			r_list_append (toremove, gn);
		}
	}

	r_list_foreach (toremove, it, gn) {
		r_graph_del_node (g->graph, gn);
	}

	r_list_free (toremove);
}

/* 1) trasform the graph into a DAG
 * 2) partition the nodes in layers
 * 3) split long edges that traverse multiple layers
 * 4) reorder nodes in each layer to reduce the number of edge crossing
 * 5) assign x and y coordinates to each node (TODO)
 * 6) restore the original graph, with long edges and cycles */
static void set_layout_bb(AGraph *g) {
	int i, j, k, rh, nx;

	remove_cycles (g);
	assign_layers (g);
	create_dummy_nodes (g);
	create_layers (g);
	minimize_crossings (g);

	/* identify row height */
	for (i = 0; i < g->n_layers; i++) {
		rh = 0;
		for (j = 0; j < g->layers[i].n_nodes; ++j) {
			ANode *n = (ANode *)(g->layers[i].nodes[j]->data);
			if (n->h > rh)
				rh = n->h;
		}
		g->layers[i].height = rh;
	}

	/* vertical align */
	for (i = 0; i < g->n_layers; ++i) {
		for (j = 0; j < g->layers[i].n_nodes; ++j) {
			ANode *n = (ANode *)(g->layers[i].nodes[j]->data);
			n->y = 1;
			for (k = 0; k < n->layer; ++k) {
				n->y += g->layers[k].height + VERTICAL_NODE_SPACING;
			}
		}
	}

	/* TODO: x-coordinate assignment algorithm */
	/* horizontal align */
	for (i = 0; i < g->n_layers; i++) {
		nx = (i % 2) * 10;
		for (j = 0; j < g->layers[i].n_nodes; ++j) {
			ANode *n = (ANode *)(g->layers[i].nodes[j]->data);
			n->x = nx;
			nx += n->w + HORIZONTAL_NODE_SPACING;
		}
	}

	restore_original_edges (g);
	remove_dummy_nodes (g);

	/* free all temporary structures used during layout */
	for (i = 0; i < g->n_layers; ++i)
		free (g->layers[i].nodes);
	free (g->layers);
	r_list_free (g->long_edges);
	r_list_free (g->back_edges);
}

static void set_layout_callgraph(AGraph *g) {
	const RList *nodes = r_graph_get_nodes (g->graph);
	RGraphNode *gn;
	RListIter *it;
	ANode *prev_n = NULL, *n;
	int y = 5, x = 20;

	graph_foreach_anode (nodes, it, gn, n) {
		// wrap to width 'w'
		if (prev_n && n->x < prev_n->x) {
			y += 10;
			x = 0;
		}
		n->x = x;
		n->y = prev_n ? y : 2;
		x += 30;
		prev_n = n;
	}
}

/* build the RGraph inside the AGraph g, starting from the Basic Blocks */
static int get_bbnodes(AGraph *g) {
	RAnalBlock *bb;
	RListIter *iter;
	Sdb *g_nodes = sdb_new0 ();
	if (!g_nodes) return R_FALSE;

	r_list_foreach (g->fcn->bbs, iter, bb) {
		RGraphNode *gn;
		ANode *node;

		if (bb->addr == UT64_MAX)
			continue;

		node = R_NEW0 (ANode);
		if (!node) {
			sdb_free (g_nodes);
			return R_FALSE;
		}

		if (g->is_simple_mode) {
			node->text = r_core_cmd_strf (g->core,
					"pI %d @ 0x%08"PFMT64x, bb->size, bb->addr);
		}else {
			node->text = r_core_cmd_strf (g->core,
					"pDi %d @ 0x%08"PFMT64x, bb->size, bb->addr);
		}
		node->addr = bb->addr;
		node->layer = -1;
		node->pos_in_layer = -1;
		node->is_dummy = R_FALSE;
		node->x = 0;
		node->y = 0;
		node->w = 0;
		node->h = 0;

		gn = r_graph_add_node (g->graph, node);
		if (!gn) { 
			sdb_free (g_nodes);
			return R_FALSE;
		}
		hash_set (g_nodes, bb->addr, gn);
	}

	r_list_foreach (g->fcn->bbs, iter, bb) {
		RGraphNode *u, *v;
		if (bb->addr == UT64_MAX)
			continue;

		u = hash_get_rnode (g_nodes, bb->addr);
		if (bb->jump != UT64_MAX) {
			v = hash_get_rnode (g_nodes, bb->jump);
			r_graph_add_edge (g->graph, u, v);
		}
		if (bb->fail != UT64_MAX) {
			v = hash_get_rnode (g_nodes, bb->fail);
			r_graph_add_edge (g->graph, u, v);
		}
	}

	g->curnode = r_list_iterator (r_graph_get_nodes(g->graph));
	sdb_free (g_nodes);
	return R_TRUE;
}

/* build the RGraph inside the AGraph g, starting from the Call Graph
 * information */
static int get_cgnodes(AGraph *g) {
#if FCN_OLD
	Sdb *g_nodes = sdb_new0 ();
	RGraphNode *fcn_gn;
	RListIter *iter;
	RAnalRef *ref;
	ANode *node;
	char *code;

	node = R_NEW0 (ANode);
	if (!node) { 
		sdb_free (g_nodes);
		return R_FALSE;
	}
	node->text = strdup ("");
	node->addr = g->fcn->addr;
	node->layer = -1;
	node->pos_in_layer = -1;
	node->is_dummy = R_FALSE;
	node->x = 10;
	node->y = 3;
	node->w = 0;
	node->h = 0;
	fcn_gn = r_graph_add_node (g->graph, node);
	if (!fcn_gn) { 
		sdb_free (g_nodes);
		return R_FALSE;
	}
	hash_set (g_nodes, g->fcn->addr, fcn_gn);

	r_list_foreach (g->fcn->refs, iter, ref) {
		/* XXX: something is broken, why there are duplicated
		 *      nodes here?! goto check fcn->refs!! */
		/* avoid dups wtf */
		RGraphNode *gn;
		gn = hash_get_rnode (g_nodes, ref->addr);
		if (gn) continue;

		RFlagItem *fi = r_flag_get_at (g->core->flags, ref->addr);
		node = R_NEW0 (ANode);
		if (!node) {
			sdb_free (g_nodes);
			return R_FALSE;
		}
		if (fi) {
			node->text = strdup (fi->name);
			node->text = r_str_concat (node->text, ":\n");
		} else {
			node->text = strdup ("");
		}
		code = r_core_cmd_strf (g->core,
			"pi 4 @ 0x%08"PFMT64x, ref->addr);
		node->text = r_str_concat (node->text, code);
		node->text = r_str_concat (node->text, "...\n");
		node->addr = ref->addr;
		node->layer = -1;
		node->pos_in_layer = -1;
		node->is_dummy = R_FALSE;
		node->x = 10;
		node->y = 10;
		node->w = 0;
		node->h = 0;
		free (code);
		gn = r_graph_add_node (g->graph, node);
		if (!gn) { 
			sdb_free (g_nodes);
			return R_FALSE;
		}
		hash_set (g_nodes, ref->addr, gn);

		r_graph_add_edge (g->graph, fcn_gn, gn);
	}

	g->curnode = r_list_iterator (r_graph_get_nodes (g->graph));
	sdb_free (g_nodes);
#else
	eprintf ("Must be sdbized\n");
#endif

	return R_TRUE;
}

static int reload_nodes(AGraph *g) {
	int ret;

	if (g->is_callgraph) {
		ret = get_cgnodes(g);
		if (!ret)
			return R_FALSE;
	} else {
		ret = get_bbnodes(g);
		if (!ret)
			return R_FALSE;
	}

	update_node_dimension(g->graph, g->is_small_nodes);
	return R_TRUE;
}

static void update_seek(RConsCanvas *can, ANode *n, int force) {
	int x, y, w, h;
	int doscroll = R_FALSE;

	if (!n) return;

	x = n->x + can->sx;
	y = n->y + can->sy;
	w = can->w;
	h = can->h;

	doscroll = force || y < 0 || y + 5 > h || x + 5 > w || x + n->w + 5 < 0;

	if (doscroll) {
		// top-left
		can->sy = -n->y + BORDER;
		can->sx = -n->x + BORDER;
		// center
		can->sy = -n->y + BORDER + (h / 8);
		can->sx = -n->x + BORDER + (w / 4);
	}
}

static void agraph_set_layout(AGraph *g) {
	if (g->is_callgraph)
		set_layout_callgraph(g);
	else
		set_layout_bb(g);
}

/* set the willing to center the screen on a particular node */
static void agraph_update_seek(AGraph *g, ANode *n, int force) {
	g->update_seek_on = n;
	g->force_update_seek = force;
}

static void agraph_free(AGraph *g) {
	r_graph_free (g->graph);
	r_stack_free (g->history);
	free(g);
}

static void agraph_print_node(AGraph *g, ANode *n) {
	const int cur = get_anode (g->curnode) == n;

	if (g->is_small_nodes)
		small_ANode_print(g, n, cur);
	else
		normal_ANode_print(g, n, cur);
}

static void agraph_print_nodes(AGraph *g) {
	const RList *nodes = r_graph_get_nodes (g->graph);
	RGraphNode *gn;
	RListIter *it;
	ANode *n;

	graph_foreach_anode (nodes, it, gn, n) {
		if (gn != get_gn (g->curnode))
			agraph_print_node(g, n);
	}

	/* draw current node now to make it appear on top */
	agraph_print_node (g, get_anode(g->curnode));
}

/* print an edge between two nodes.
 * nth: specifies if the edge is the true(1)/false(2) branch or if it's the
 *      only edge for that node(0), so that a different style will be applied
 *      to the drawn line */
static void agraph_print_edge(AGraph *g, ANode *a, ANode *b, int nth) {
	int x, y, x2, y2;
	int xinc = 3 + 2 * (nth + 1);
	x = a->x + xinc;
	y = a->y + a->h;
	x2 = b->x + xinc;
	y2 = b->y;
	if (a == b) {
		x2 = a->x;
		y2 = y - 3;
	}
	if (nth > 1)
		nth = 1;
	switch (nth) {
	case 0: L1 (x, y, x2, y2); break;
	case 1: L2 (x, y, x2, y2); break;
	case -1: L (x, y, x2, y2); break;
	}
}

static void agraph_print_edges(AGraph *g) {
	const RList *nodes = r_graph_get_nodes (g->graph);
	RGraphNode *gn, *gv;
	RListIter *it, *itn;
	ANode *u, *v;

	graph_foreach_anode (nodes, it, gn, u) {
		const RList *neighbours = r_graph_get_neighbours (g->graph, gn);
		const int exit_edges = r_list_length (neighbours);
		int nth = 0;

		graph_foreach_anode (neighbours, itn, gv, v) {
			int cur_nth = nth;
			if (g->is_callgraph) {
				/* hack: we don't support more than two exit edges from a node
				 * yet, so set nth to zero, to make every edge appears as the
				 * "true" edge of the node */
				cur_nth = 0;
			} else if (exit_edges == 1) {
				cur_nth = -1;
			}
			agraph_print_edge (g, u, v, cur_nth);
			nth++;
		}
	}
}

static void agraph_toggle_small_nodes(AGraph *g) {
	g->is_small_nodes = !g->is_small_nodes;
	g->need_reload_nodes = R_TRUE;
}

static void agraph_toggle_simple_mode(AGraph *g) {
	g->is_simple_mode = !g->is_simple_mode;
	g->need_reload_nodes = R_TRUE;
}

static void agraph_toggle_callgraph(AGraph *g) {
	g->is_callgraph = !g->is_callgraph;
	g->need_reload_nodes = R_TRUE;
}

/* reload all the info in the nodes, depending on the type of the graph
 * (callgraph, CFG, etc.), set the default layout for these nodes and center
 * the screen on the selected one */
static int agraph_reload_nodes(AGraph *g) {
	int ret;

	r_graph_reset (g->graph);
	ret = reload_nodes(g);
	if (!ret)
		return R_FALSE;
	agraph_set_layout(g);
	g->update_seek_on = get_anode(g->curnode);
	return R_TRUE;
}

static void follow_nth(AGraph *g, int nth) {
	const RGraphNode *cn = r_graph_nth_neighbour (g->graph, get_gn(g->curnode), nth);
	if (cn) {
		history_push (g->history, get_gn (g->curnode));
		g->curnode = r_graph_node_iter (g->graph, cn->idx);
	}
}

static void agraph_follow_true(AGraph *g) {
	follow_nth(g, 0);
	agraph_update_seek(g, get_anode(g->curnode), R_FALSE);
}

static void agraph_follow_false(AGraph *g) {
	follow_nth(g, 1);
	agraph_update_seek(g, get_anode(g->curnode), R_FALSE);
}

/* go back in the history of selected nodes, if we can */
static void agraph_undo_node(AGraph *g) {
	const RGraphNode *p = history_pop (g->history);
	if (p) {
		g->curnode = r_graph_node_iter (g->graph, p->idx);
		agraph_update_seek (g, p->data, R_FALSE);
	}
}

/* pushes the current node in the history and makes g->curnode the next node in
 * the order given by r_graph_get_nodes */
static void agraph_next_node(AGraph *g) {
	if (!g->curnode->n) return;
	history_push (g->history, get_gn(g->curnode));
	g->curnode = g->curnode->n;
	agraph_update_seek (g, get_anode(g->curnode), R_FALSE);
}

/* pushes the current node in the history and makes g->curnode the prev node in
 * the order given by r_graph_get_nodes */
static void agraph_prev_node(AGraph *g) {
	if (!g->curnode->p) return;
	history_push (g->history, get_gn(g->curnode));
	g->curnode = g->curnode->p;
	agraph_update_seek (g, get_anode(g->curnode), R_FALSE);
}

static int agraph_refresh(struct agraph_refresh_data *grd) {
	char title[TITLE_LEN];
	AGraph *g = grd->g;
	const int fs = grd->fs;
	int h, w = r_cons_get_size (&h);
	int ret;

	/* allow to change the current function only during debugging */
	if (g->is_instep && g->core->io->debug) {
		RAnalFunction *f;
		r_core_cmd0 (g->core, "sr pc");
		f = r_anal_get_fcn_in (g->core->anal, g->core->offset, 0);
		if (f && f != g->fcn) {
			g->fcn = f;
			g->need_reload_nodes = R_TRUE;
		}
	}

	/* look for any change in the state of the graph
	 * and update what's necessary */
	if (g->need_reload_nodes) {
		ret = agraph_reload_nodes(g);
		if (!ret)
			return R_FALSE;

		g->need_reload_nodes = R_FALSE;
	}
	if (g->update_seek_on) {
		update_seek(g->can, g->update_seek_on, g->force_update_seek);
		g->update_seek_on = NULL;
		g->force_update_seek = R_FALSE;
	}

	if (fs) {
		r_cons_clear00 ();
	}

	h = fs ? h : 1024;
	r_cons_canvas_resize (g->can, w, h);
	r_cons_canvas_clear (g->can);

	agraph_print_edges(g);
	agraph_print_nodes(g);

	if (fs) {
		(void)G (-g->can->sx, -g->can->sy);
		snprintf (title, sizeof (title)-1,
			"[0x%08"PFMT64x"]> %d VV @ %s (nodes %d edges %d zoom %d%%) %s mouse:%s",
			g->fcn->addr, r_stack_size (g->history), g->fcn->name,
			g->graph->n_nodes, g->graph->n_edges, zoom, g->is_callgraph?"CG":"BB",
			mousemodes[mousemode]);
		W (title);
	}

	if (fs) {
		r_cons_canvas_print (g->can);
	} else {
		r_cons_canvas_print_region (g->can);
	}
	if (fs) {
		const char *cmdv = r_config_get (g->core->config, "cmd.gprompt");
		if (cmdv && *cmdv) {
			r_cons_gotoxy (0, 1);
			r_core_cmd0 (g->core, cmdv);
		}
	}
	r_cons_flush_nonewline ();
	return R_TRUE;
}

static void agraph_init(AGraph *g) {
	g->is_callgraph = R_FALSE;
	g->is_instep = R_FALSE;
	g->is_simple_mode = R_TRUE;
	g->is_small_nodes = R_FALSE;
	g->need_reload_nodes = R_TRUE;
	g->curnode = NULL;
	g->update_seek_on = NULL;
	g->force_update_seek = R_TRUE;
	g->history = r_stack_new (INIT_HISTORY_CAPACITY);
	g->graph = r_graph_new ();
	g->back_edges = NULL;
}

static AGraph *agraph_new(RCore *core, RConsCanvas *can, RAnalFunction *fcn) {
	AGraph *g = R_NEW0 (AGraph);
	if (!g) return NULL;

	g->core = core;
	g->can = can;
	g->fcn = fcn;

	agraph_init(g);
	return g;
}

R_API int r_core_visual_graph(RCore *core, RAnalFunction *_fcn, int is_interactive) {
	int exit_graph = R_FALSE, is_error = R_FALSE;
	struct agraph_refresh_data *grd;
	int okey, key, wheel;
	RAnalFunction *fcn;
	const char *key_s;
	RConsCanvas *can;
	AGraph *g;
	int wheelspeed;
	int w, h;
	int ret;

	fcn = _fcn? _fcn: r_anal_get_fcn_in (core->anal, core->offset, 0);
	if (!fcn) {
		eprintf ("No function in current seek\n");
		return R_FALSE;
	}
	w = r_cons_get_size (&h);
	can = r_cons_canvas_new (w, h);
	if (!can) {
		eprintf ("Cannot create RCons.canvas context\n");
		return R_FALSE;
	}
	can->linemode = 1;
	can->color = r_config_get_i (core->config, "scr.color");
	// disable colors in disasm because canvas doesnt supports ansi text yet
	r_config_set_i (core->config, "scr.color", 0);

	g = agraph_new (core, can, fcn);
	if (!g) {
		is_error = R_TRUE;
		goto err_graph_new;
	}

	grd = (struct agraph_refresh_data *)malloc (sizeof(*grd));
	grd->g = g;
	grd->fs = is_interactive;

	core->cons->event_data = grd;
	core->cons->event_resize = (RConsEvent)agraph_refresh;

	while (!exit_graph && !is_error) {
		w = r_cons_get_size (&h);
		ret = agraph_refresh (grd);
		if (!ret) {
			is_error = R_TRUE;
			break;
		}

		if (!is_interactive) {
			/* this is a non-interactive ascii-art graph, so exit the loop */
			r_cons_printf (Color_RESET);
			break;
		}

		r_cons_show_cursor(R_FALSE);
		wheel = r_config_get_i (core->config, "scr.wheel");
		if (wheel)
			r_cons_enable_mouse (R_TRUE);

		// r_core_graph_inputhandle()
		okey = r_cons_readchar ();
		key = r_cons_arrow_to_hjkl (okey);
		wheelspeed = r_config_get_i (core->config, "scr.wheelspeed");

		switch (key) {
		case '-':
			zoom-=10;
			if (zoom<0) zoom = 0;
			update_node_dimension(g->graph, g->is_small_nodes);
			agraph_set_layout (g);
			//  agraph_update_seek (g, get_anode(g->curnode), R_TRUE);
			break;
		case '+':
			zoom+=10;
			update_node_dimension (g->graph, g->is_small_nodes);
			agraph_set_layout (g);
			//  agraph_update_seek (g, get_anode(g->curnode), R_TRUE);
			 // g->is_instep = R_TRUE;
			break;
		case '=':
		case '|':
			{ // TODO: edit
				const char *buf = NULL;
				const char *cmd = r_config_get (core->config, "cmd.gprompt");
				r_line_set_prompt ("cmd.gprompt> ");
				core->cons->line->contents = strdup (cmd);
				buf = r_line_readline ();
				core->cons->line->contents = NULL;
				r_config_set (core->config, "cmd.gprompt", buf);
			}
			break;
		case 'O':
			agraph_toggle_simple_mode(g);
			break;
		case 'V':
			agraph_toggle_callgraph(g);
			break;
		case 'z':
			g->is_instep = R_TRUE;
			key_s = r_config_get (core->config, "key.s");
			if (key_s && *key_s) {
				r_core_cmd0 (core, key_s);
			} else {
				if (r_config_get_i (core->config, "cfg.debug"))
					r_core_cmd0 (core, "ds;.dr*");
				else
					r_core_cmd0 (core, "aes;.dr*");
			}
			ret = agraph_reload_nodes(g);
			if (!ret)
				is_error = R_TRUE;
			break;
		case 'Z':
			if (okey == 27) {
				agraph_prev_node(g);
			} else {
				// 'Z'
				g->is_instep = R_TRUE;
				if (r_config_get_i (core->config, "cfg.debug"))
					r_core_cmd0 (core, "dso;.dr*");
				else
					r_core_cmd0 (core, "aeso;.dr*");

				ret = agraph_reload_nodes(g);
				if (!ret)
					is_error = R_TRUE;
			}
			break;
		case 'x':
			if (r_core_visual_xrefs_x (core))
				exit_graph = R_TRUE;
			break;
		case 'X':
			if (r_core_visual_xrefs_X (core))
				exit_graph = R_TRUE;
			break;
		case 9: // tab
			agraph_next_node(g);
			break;
		case '?':
			r_cons_clear00 ();
			r_cons_printf ("Visual Ascii Art graph keybindings:\n"
					" .    - center graph to the current node\n"
					" C    - toggle scr.color\n"
					" hjkl - move node\n"
					" HJKL - scroll canvas\n"
					" tab  - select next node\n"
					" TAB  - select previous node\n"
					" t/f  - follow true/false edges\n"
					" e    - toggle edge-lines style (diagonal/square)\n"
					" O    - toggle disasm mode\n"
					" p    - toggle mini-graph\n"
					" u    - select previous node\n"
					" V    - toggle basicblock / call graphs\n"
					" x/X  - jump to xref/ref\n"
					" z/Z  - step / step over\n"
					" +/-  - zoom in/out\n"
					" R    - relayout\n");
			r_cons_flush ();
			r_cons_any_key (NULL);
			break;
		case 'R':
		case 'r':
			agraph_set_layout (g);
			break;
		case 'j':
			if (r_cons_singleton()->mouse_event) {
				switch (mousemode) {
					case 0: // canvas-y
						can->sy += wheelspeed;
						break;
					case 1: // canvas-x
						can->sx += wheelspeed;
						break;
					case 2: // node-y
						get_anode(g->curnode)->y += wheelspeed;
						break;
					case 3: // node-x
						get_anode(g->curnode)->x += wheelspeed;
						break;
				}
			} else {
				get_anode(g->curnode)->y++;
			}
			break;
		case 'k':
			if (r_cons_singleton()->mouse_event) {
				switch (mousemode) {
					case 0: // canvas-y
						can->sy -= wheelspeed;
						break;
					case 1: // canvas-x
						can->sx -= wheelspeed;
						break;
					case 2: // node-y
						get_anode(g->curnode)->y -= wheelspeed;
						break;
					case 3: // node-x
						get_anode(g->curnode)->x -= wheelspeed;
						break;
				}
			} else {
				get_anode(g->curnode)->y--;
			}
			break;
		case 'm':
			mousemode++;
			if (!mousemodes[mousemode])
				mousemode = 0;
			break;
		case 'M':
			mousemode--;
			if (mousemode<0)
				mousemode = 3;
			break;
		case 'h': get_anode(g->curnode)->x--; break;
		case 'l': get_anode(g->curnode)->x++; break;

		case '0': can->sx = can->sy = 0; break;
		case 'K': can->sy -= wheelspeed; break;
		case 'J': can->sy += wheelspeed; break;
		case 'H': can->sx -= wheelspeed; break;
		case 'L': can->sx += wheelspeed; break;

		case 'e':
			  can->linemode = !!!can->linemode;
			  break;
		case 'p':
			  agraph_toggle_small_nodes(g);
			  break;
		case 'u':
			  agraph_undo_node(g);
			  break;
		case '.':
			  zoom = 100;
			  update_node_dimension (g->graph, g->is_small_nodes);
			  agraph_set_layout (g);
			  //agraph_update_seek (g, get_anode (g->curnode), R_TRUE);
			  g->is_instep = R_TRUE;
			  break;
		case 't':
			  agraph_follow_true (g);
			  break;
		case 'f':
			  agraph_follow_false (g);
			  break;
		case '/':
			  r_core_cmd0 (core, "?i highlight;e scr.highlight=`?y`");
			  break;
		case ':':
			  core->vmode = R_FALSE;
			  r_core_visual_prompt_input (core);
			  core->vmode = R_TRUE;
			  break;
		case 'C':
			  can->color = !!!can->color;
			  //r_config_swap (core->config, "scr.color");
			  // refresh graph
			  break;
		case -1: // EOF
		case 'q':
			  exit_graph = R_TRUE;
			  break;
		case 27: // ESC
			  if (r_cons_readchar () == 91) {
				  if (r_cons_readchar () == 90) {
					  agraph_prev_node (g);
				  }
			  }
			  break;
		default:
			  eprintf ("Key %d\n", key);
			  //sleep (1);
			  break;
		}
	}

	agraph_free(g);
err_graph_new:
	r_config_set_i (core->config, "scr.color", can->color);
	free (can);
	return !is_error;
}
