#define PY_SSIZE_T_CLEAN
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <Python.h>
#include <math.h>
#include <geos_c.h>
#include <structmember.h>
#include "numpy/ndarraytypes.h"
#include "numpy/ufuncobject.h"
#include "numpy/npy_3kcompat.h"

#include "fast_loop_macros.h"

/* This tells Python what methods this module has. */
static PyMethodDef GeosModule[] = {
    {NULL, NULL, 0, NULL},
    {NULL, NULL, 0, NULL}
};

/* This initializes a global GEOS Context */
static void *geos_context[1] = {NULL};

static void HandleGEOSError(const char *message, void *userdata) {
    PyErr_SetString(userdata, message);
}

static void HandleGEOSNotice(const char *message, void *userdata) {
    PyErr_WarnEx(PyExc_Warning, message, 1);
}


static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "ufuncs",
    NULL,
    -1,
    GeosModule,
    NULL,
    NULL,
    NULL,
    NULL
};

typedef struct {
    PyObject_HEAD;
    void *ptr;
    char geom_type_id;
    char has_z;
} GeometryObject;


static PyObject *GeometryObject_new_from_ptr(
    PyTypeObject *type, void *context_handle, GEOSGeometry *ptr)
{
    GeometryObject *self;
    int geos_result;
    self = (GeometryObject *) type->tp_alloc(type, 0);

    if (self != NULL) {
        self->ptr = ptr;
        geos_result = GEOSGeomTypeId_r(context_handle, ptr);
        if ((geos_result < 0) | (geos_result > 255)) {
            goto fail;
        }
        self->geom_type_id = geos_result;
        geos_result = GEOSHasZ_r(context_handle, ptr);
        if ((geos_result < 0) | (geos_result > 1)) {
            goto fail;
        }
        self->has_z = geos_result;
    }
    return (PyObject *) self;
    fail:
        PyErr_SetString(PyExc_RuntimeError, "Geometry initialization failed");
        Py_DECREF(self);
        return NULL;
}


static PyObject *GeometryObject_new(PyTypeObject *type, PyObject *args,
                                    PyObject *kwds)
{
    void *context_handle;
    GEOSGeometry *ptr;
    PyObject *self;
    long arg;

    if (!PyArg_ParseTuple(args, "l", &arg)) {
        goto fail;
    }
    context_handle = geos_context[0];
    ptr = GEOSGeom_clone_r(context_handle, (GEOSGeometry *) arg);
    if (ptr == NULL) {
        /* GEOS_finish_r(context_handle); */
        goto fail;
    }
    self = GeometryObject_new_from_ptr(type, context_handle, ptr);
    /* GEOS_finish_r(context_handle); */
    return (PyObject *) self;

    fail:
        PyErr_SetString(PyExc_ValueError, "Please provide a C pointer to a GEOSGeometry");
        return NULL;
}

static void GeometryObject_dealloc(GeometryObject *self)
{
    void *context_handle;
    if (self->ptr != NULL) {
        context_handle = geos_context[0];
        GEOSGeom_destroy_r(context_handle, self->ptr);
        /* GEOS_finish_r(context_handle); */
    }
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyMemberDef GeometryObject_members[] = {
    {"ptr", T_INT, offsetof(GeometryObject, ptr), 0, "pointer to GEOSGeometry"},
    {"geom_type_id", T_INT, offsetof(GeometryObject, geom_type_id), 0, "geometry type ID"},
    {"has_z", T_INT, offsetof(GeometryObject, has_z), 0, "has Z"},
    {NULL}  /* Sentinel */
};

static PyTypeObject GeometryType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pygeos.ufuncs.GEOSGeometry",
    .tp_doc = "Geometry type",
    .tp_basicsize = sizeof(GeometryObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = GeometryObject_new,
    .tp_dealloc = (destructor) GeometryObject_dealloc,
    .tp_members = GeometryObject_members,
};

#define RAISE_ILLEGAL_GEOS  /* PyErr_Format(PyExc_RuntimeError, "GEOS Operation failed") */
#define CREATE_COORDSEQ(SIZE, NDIM)\
    void *coord_seq = GEOSCoordSeq_create_r(context_handle, SIZE, NDIM);\
    if (coord_seq == NULL) {\
        return;\
    }

#define SET_COORD(N, DIM)\
    if (!GEOSCoordSeq_setOrdinate_r(context_handle, coord_seq, N, DIM, coord)) {\
        GEOSCoordSeq_destroy_r(context_handle, coord_seq);\
        RAISE_ILLEGAL_GEOS;\
        return;\
    }

#define CHECK_GEOM(GEOM)\
    if (!PyObject_IsInstance((PyObject *) GEOM, (PyObject *) &GeometryType)) {\
        PyErr_Format(PyExc_TypeError, "One of the arguments is of incorrect type. Please provide only Geometry objects.");\
        return;\
    }\
    if (GEOM->ptr == NULL) {\
        PyErr_Format(PyExc_ValueError, "A geometry object is empty");\
        return;\
    }


#define INPUT_Y\
    GeometryObject *in1 = *(GeometryObject **)ip1;\
    CHECK_GEOM(in1)

#define INPUT_YY\
    GeometryObject *in1 = *(GeometryObject **)ip1;\
    GeometryObject *in2 = *(GeometryObject **)ip2;\
    CHECK_GEOM(in1);\
    CHECK_GEOM(in2)

#define OUTPUT_b\
    if (! ((ret == 0) || (ret == 1))) {\
        RAISE_ILLEGAL_GEOS;\
        return;\
    }\
    *(npy_bool *)op1 = ret

#define OUTPUT_Y\
    if (ret_ptr == NULL) {\
        RAISE_ILLEGAL_GEOS;\
        return;\
    }\
    PyObject *ret = GeometryObject_new_from_ptr(&GeometryType, context_handle, ret_ptr);\
    if (ret == NULL) {\
        PyErr_Format(PyExc_RuntimeError, "Could not instantiate a new Geometry object");\
        return;\
    }\
    PyObject **out = (PyObject **)op1;\
    Py_XDECREF(*out);\
    *out = ret

/* Define the geom -> bool functions (YY_b) */
static void *is_empty_data[1] = {GEOSisEmpty_r};
static void *is_simple_data[1] = {GEOSisSimple_r};
static void *is_ring_data[1] = {GEOSisRing_r};
static void *has_z_data[1] = {GEOSHasZ_r};
static void *is_closed_data[1] = {GEOSisClosed_r};
static void *is_valid_data[1] = {GEOSisValid_r};
typedef char FuncGEOS_Y_b(void *context, void *a);
static char Y_b_dtypes[2] = {NPY_OBJECT, NPY_BOOL};
static void Y_b_func(char **args, npy_intp *dimensions,
                     npy_intp *steps, void *data)
{
    FuncGEOS_Y_b *func = (FuncGEOS_Y_b *)data;
    void *context_handle = geos_context[0];

    UNARY_LOOP {
        INPUT_Y;
        npy_bool ret = func(context_handle, in1->ptr);
        OUTPUT_b;
    }
}
static PyUFuncGenericFunction Y_b_funcs[1] = {&Y_b_func};


/* Define the geom, geom -> bool functions (YY_b) */
static void *disjoint_data[1] = {GEOSDisjoint_r};
static void *touches_data[1] = {GEOSTouches_r};
static void *intersects_data[1] = {GEOSIntersects_r};
static void *crosses_data[1] = {GEOSCrosses_r};
static void *within_data[1] = {GEOSWithin_r};
static void *contains_data[1] = {GEOSContains_r};
static void *overlaps_data[1] = {GEOSOverlaps_r};
static void *equals_data[1] = {GEOSEquals_r};
static void *covers_data[1] = {GEOSCovers_r};
static void *covered_by_data[1] = {GEOSCoveredBy_r};
typedef char FuncGEOS_YY_b(void *context, void *a, void *b);
static char YY_b_dtypes[3] = {NPY_OBJECT, NPY_OBJECT, NPY_BOOL};
static void YY_b_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    FuncGEOS_YY_b *func = (FuncGEOS_YY_b *)data;
    void *context_handle = geos_context[0];

    BINARY_LOOP {
        INPUT_YY;
        npy_bool ret = func(context_handle, in1->ptr, in2->ptr);
        OUTPUT_b;
    }
}
static PyUFuncGenericFunction YY_b_funcs[1] = {&YY_b_func};

/* Define the geom -> geom functions (Y_Y) */
static void *clone_data[1] = {GEOSGeom_clone_r};
static void *envelope_data[1] = {GEOSEnvelope_r};
static void *convex_hull_data[1] = {GEOSConvexHull_r};
static void *boundary_data[1] = {GEOSBoundary_r};
static void *unary_union_data[1] = {GEOSUnaryUnion_r};
static void *point_on_surface_data[1] = {GEOSPointOnSurface_r};
static void *get_centroid_data[1] = {GEOSGetCentroid_r};
static void *line_merge_data[1] = {GEOSLineMerge_r};
static void *extract_unique_points_data[1] = {GEOSGeom_extractUniquePoints_r};
static void *get_start_point_data[1] = {GEOSGeomGetStartPoint_r};
static void *get_end_point_data[1] = {GEOSGeomGetEndPoint_r};
static void *get_exterior_ring_data[1] = {GEOSGetExteriorRing_r};
/* the normalize funcion acts inplace */
static void *GEOSNormalize_r_with_clone(void *context, void *geom) {
    void *ret = GEOSGeom_clone_r(context, geom);
    if (ret == NULL) {
        return NULL;
    }
    if (GEOSNormalize_r(context, geom) == -1) {
        return NULL;
    }
    return ret;
}
static void *normalize_data[1] = {GEOSNormalize_r_with_clone};
/* a linear-ring to polygon conversion function */
static void *GEOSLinearRingToPolygon(void *context, void *geom) {
    void *shell = GEOSGeom_clone_r(context, geom);
    if (shell == NULL) {
        return NULL;
    }
    return GEOSGeom_createPolygon_r(context, shell, NULL, 0);
}
static void *polygons_without_holes_data[1] = {GEOSLinearRingToPolygon};
typedef void *FuncGEOS_Y_Y(void *context, void *a);
static char Y_Y_dtypes[2] = {NPY_OBJECT, NPY_OBJECT};
static void Y_Y_func(char **args, npy_intp *dimensions,
                     npy_intp *steps, void *data)
{
    FuncGEOS_Y_Y *func = (FuncGEOS_Y_Y *)data;
    void *context_handle = geos_context[0];

    UNARY_LOOP {
        INPUT_Y;
        GEOSGeometry *ret_ptr = func(context_handle, in1->ptr);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction Y_Y_funcs[1] = {&Y_Y_func};

/* Define the geom, double -> geom functions (Yd_Y) */
static void *interpolate_data[1] = {GEOSInterpolate_r};
static void *interpolate_normalized_data[1] = {GEOSInterpolateNormalized_r};
static void *simplify_data[1] = {GEOSSimplify_r};
static void *topology_preserve_simplify_data[1] = {GEOSTopologyPreserveSimplify_r};
typedef void *FuncGEOS_Yd_Y(void *context, void *a, double b);
static char Yd_Y_dtypes[3] = {NPY_OBJECT, NPY_DOUBLE, NPY_OBJECT};
static void Yd_Y_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    FuncGEOS_Yd_Y *func = (FuncGEOS_Yd_Y *)data;
    void *context_handle = geos_context[0];

    BINARY_LOOP {
        INPUT_Y;
        double in2 = *(double *)ip2;
        GEOSGeometry *ret_ptr = func(context_handle, in1->ptr, in2);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction Yd_Y_funcs[1] = {&Yd_Y_func};

/* Define the geom, int -> geom functions (Yl_Y) */
static void *get_interior_ring_n_data[1] = {GEOSGetInteriorRingN_r};
static void *get_point_n_data[1] = {GEOSGeomGetPointN_r};
static void *get_geometry_n_data[1] = {GEOSGetGeometryN_r};
typedef void *FuncGEOS_Yi_Y(void *context, void *a, int b);
static char Yi_Y_dtypes[3] = {NPY_OBJECT, NPY_INT, NPY_OBJECT};
static void Yi_Y_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    FuncGEOS_Yi_Y *func = (FuncGEOS_Yi_Y *)data;
    void *context_handle = geos_context[0];

    BINARY_LOOP {
        INPUT_Y;
        int in2 = *(int *)ip2;
        GEOSGeometry *ret_ptr = func(context_handle, in1->ptr, in2);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction Yi_Y_funcs[1] = {&Yi_Y_func};

/* Define the geom, geom -> geom functions (YY_Y) */
static void *intersection_data[1] = {GEOSIntersection_r};
static void *difference_data[1] = {GEOSDifference_r};
static void *symmetric_difference_data[1] = {GEOSSymDifference_r};
static void *union_data[1] = {GEOSUnion_r};
static void *shared_paths_data[1] = {GEOSSharedPaths_r};
typedef void *FuncGEOS_YY_Y(void *context, void *a, void *b);
static char YY_Y_dtypes[3] = {NPY_OBJECT, NPY_OBJECT, NPY_OBJECT};
static void YY_Y_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    FuncGEOS_YY_Y *func = (FuncGEOS_YY_Y *)data;
    void *context_handle = geos_context[0];

    BINARY_LOOP {
        INPUT_YY;
        GEOSGeometry *ret_ptr = func(context_handle, in1->ptr, in2->ptr);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction YY_Y_funcs[1] = {&YY_Y_func};

/* Define the geom -> double functions (Y_d) */
static void *get_x_data[1] = {GEOSGeomGetX_r};
static void *get_y_data[1] = {GEOSGeomGetY_r};
static void *area_data[1] = {GEOSArea_r};
static void *length_data[1] = {GEOSLength_r};
static void *get_length_data[1] = {GEOSGeomGetLength_r};
typedef int FuncGEOS_Y_d(void *context, void *a, double *b);
static char Y_d_dtypes[2] = {NPY_OBJECT, NPY_DOUBLE};
static void Y_d_func(char **args, npy_intp *dimensions,
                     npy_intp *steps, void *data)
{
    FuncGEOS_Y_d *func = (FuncGEOS_Y_d *)data;
    void *context_handle = geos_context[0];

    UNARY_LOOP {
        INPUT_Y;
        if (func(context_handle, in1->ptr, (npy_double *) op1) == 0) {
            RAISE_ILLEGAL_GEOS;
            return;
        }
    }
}
static PyUFuncGenericFunction Y_d_funcs[1] = {&Y_d_func};

/* Define the geom -> unsigned byte functions (Y_B) */
static void *geom_type_id_data[1] = {GEOSGeomTypeId_r};
static void *get_dimensions_data[1] = {GEOSGeom_getDimensions_r};
static void *get_coordinate_dimensions_data[1] = {GEOSGeom_getCoordinateDimension_r};
typedef int FuncGEOS_Y_B(void *context, void *a);
static char Y_B_dtypes[2] = {NPY_OBJECT, NPY_UBYTE};
static void Y_B_func(char **args, npy_intp *dimensions,
                     npy_intp *steps, void *data)
{
    FuncGEOS_Y_B *func = (FuncGEOS_Y_B *)data;
    void *context_handle = geos_context[0];
    int ret;

    UNARY_LOOP {
        INPUT_Y;
        ret = func(context_handle, in1->ptr);
        if ((ret < 0) | (ret > NPY_MAX_UBYTE)) {
            RAISE_ILLEGAL_GEOS;
            return;
        }
        *(npy_ubyte *)op1 = ret;
    }
}
static PyUFuncGenericFunction Y_B_funcs[1] = {&Y_B_func};

/* Define the geom -> int functions (Y_i) */
static void *get_srid_data[1] = {GEOSGetSRID_r};
static void *get_num_geometries_data[1] = {GEOSGetNumGeometries_r};
static void *get_num_interior_rings_data[1] = {GEOSGetNumInteriorRings_r};
static void *get_num_points_data[1] = {GEOSGeomGetNumPoints_r};
static void *get_num_coordinates_data[1] = {GEOSGetNumCoordinates_r};
typedef int FuncGEOS_Y_i(void *context, void *a);
static char Y_i_dtypes[2] = {NPY_OBJECT, NPY_INT};
static void Y_i_func(char **args, npy_intp *dimensions,
                     npy_intp *steps, void *data)
{
    FuncGEOS_Y_i *func = (FuncGEOS_Y_i *)data;
    void *context_handle = geos_context[0];
    int ret;

    UNARY_LOOP {
        INPUT_Y;
        ret = func(context_handle, in1->ptr);
        if ((ret < 0) | (ret > NPY_MAX_INT)) {
            RAISE_ILLEGAL_GEOS;
            return;
        }
        *(npy_int *)op1 = ret;
    }
}
static PyUFuncGenericFunction Y_i_funcs[1] = {&Y_i_func};

/* Define the geom, geom -> double functions (YY_d) */
static void *distance_data[1] = {GEOSDistance_r};
static void *hausdorff_distance_data[1] = {GEOSHausdorffDistance_r};
typedef int FuncGEOS_YY_d(void *context, void *a,  void *b, double *c);
static char YY_d_dtypes[3] = {NPY_OBJECT, NPY_OBJECT, NPY_DOUBLE};
static void YY_d_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    FuncGEOS_YY_d *func = (FuncGEOS_YY_d *)data;
    void *context_handle = geos_context[0];

    BINARY_LOOP {
        INPUT_YY;
        if (func(context_handle, in1->ptr, in2->ptr, (double *) op1) == 0) {
            RAISE_ILLEGAL_GEOS;
            return;
        }
    }
}
static PyUFuncGenericFunction YY_d_funcs[1] = {&YY_d_func};

/* Define the geom, geom -> double functions that have different GEOS call signature (YY_d_2) */
static void *project_data[1] = {GEOSProject_r};
static void *project_normalized_data[1] = {GEOSProjectNormalized_r};
typedef double FuncGEOS_YY_d_2(void *context, void *a, void *b);
static char YY_d_2_dtypes[3] = {NPY_OBJECT, NPY_OBJECT, NPY_DOUBLE};
static void YY_d_2_func(char **args, npy_intp *dimensions,
                        npy_intp *steps, void *data)
{
    FuncGEOS_YY_d_2 *func = (FuncGEOS_YY_d_2 *)data;
    void *context_handle = geos_context[0];
    double ret;

    BINARY_LOOP {
        INPUT_YY;
        ret = func(context_handle, in1->ptr, in2->ptr);
        if (ret == -1.0) {
            RAISE_ILLEGAL_GEOS;
            return;
        }
        *(npy_double *) op1 = ret;
    }
}
static PyUFuncGenericFunction YY_d_2_funcs[1] = {&YY_d_2_func};

/* Define functions with unique call signatures */
static void *null_data[1] = {NULL};
static char buffer_dtypes[4] = {NPY_OBJECT, NPY_DOUBLE, NPY_INT, NPY_OBJECT};
static void buffer_func(char **args, npy_intp *dimensions,
                        npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];

    TERNARY_LOOP {
        INPUT_Y;
        double in2 = *(double *) ip2;
        int in3 = *(int *) ip3;
        GEOSGeometry *ret_ptr = GEOSBuffer_r(context_handle, in1->ptr, in2, in3);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction buffer_funcs[1] = {&buffer_func};

static char snap_dtypes[4] = {NPY_OBJECT, NPY_OBJECT, NPY_DOUBLE, NPY_OBJECT};
static void snap_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];

    TERNARY_LOOP {
        INPUT_YY;
        double in3 = *(double *) ip3;
        GEOSGeometry *ret_ptr = GEOSSnap_r(context_handle, in1->ptr, in2->ptr, in3);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction snap_funcs[1] = {&snap_func};

static char equals_exact_dtypes[4] = {NPY_OBJECT, NPY_OBJECT, NPY_DOUBLE, NPY_BOOL};
static void equals_exact_func(char **args, npy_intp *dimensions,
                      npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];

    TERNARY_LOOP {
        INPUT_YY;
        double in3 = *(double *) ip3;
        npy_bool ret = GEOSEqualsExact_r(context_handle, in1->ptr, in2->ptr, in3);
        OUTPUT_b;
    }
}
static PyUFuncGenericFunction equals_exact_funcs[1] = {&equals_exact_func};

/* define double -> geometry construction functions */

static char points_dtypes[2] = {NPY_DOUBLE, NPY_OBJECT};
static void points_func(char **args, npy_intp *dimensions,
                        npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];
    SINGLE_COREDIM_LOOP_OUTER {
        CREATE_COORDSEQ(1, n_c1);
        SINGLE_COREDIM_LOOP_INNER {
            double coord = *(double *) cp1;
            SET_COORD(0, i_c1);
        }
        GEOSGeometry *ret_ptr = GEOSGeom_createPoint_r(context_handle, coord_seq);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction points_funcs[1] = {&points_func};


static char linestrings_dtypes[2] = {NPY_DOUBLE, NPY_OBJECT};
static void linestrings_func(char **args, npy_intp *dimensions,
                              npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];
    DOUBLE_COREDIM_LOOP_OUTER {
        CREATE_COORDSEQ(n_c1, n_c2);
        DOUBLE_COREDIM_LOOP_INNER_1 {
            DOUBLE_COREDIM_LOOP_INNER_2 {
                double coord = *(double *) cp2;
                SET_COORD(i_c1, i_c2);
            }
        }
        GEOSGeometry *ret_ptr = GEOSGeom_createLineString_r(context_handle, coord_seq);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction linestrings_funcs[1] = {&linestrings_func};


static char linearrings_dtypes[2] = {NPY_DOUBLE, NPY_OBJECT};
static void linearrings_func(char **args, npy_intp *dimensions,
                             npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];
    DOUBLE_COREDIM_LOOP_OUTER {
        /* check if first and last coords are equal; duplicate if necessary */
        char ring_closure = 0;
        DOUBLE_COREDIM_LOOP_INNER_2 {
            double first_coord = *(double *) (ip1 + i_c2 * cs2);
            double last_coord = *(double *) (ip1 + (n_c1 - 1) * cs1 + i_c2 * cs2);
            if (first_coord != last_coord) {
                ring_closure = 1;
                break;
            }
        }
        /* fill the coordinate sequence */
        CREATE_COORDSEQ(n_c1 + ring_closure, n_c2);
        DOUBLE_COREDIM_LOOP_INNER_1 {
            DOUBLE_COREDIM_LOOP_INNER_2 {
                double coord = *(double *) cp2;
                SET_COORD(i_c1, i_c2);
            }
        }
        /* add the closing coordinate if necessary */
        if (ring_closure) {
            DOUBLE_COREDIM_LOOP_INNER_2 {
                double coord = *(double *) (ip1 + i_c2 * cs2);
                SET_COORD(n_c1, i_c2);
            }
        }
        GEOSGeometry *ret_ptr = GEOSGeom_createLinearRing_r(context_handle, coord_seq);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction linearrings_funcs[1] = {&linearrings_func};


static char polygons_with_holes_dtypes[3] = {NPY_OBJECT, NPY_OBJECT, NPY_OBJECT};
static void polygons_with_holes_func(char **args, npy_intp *dimensions,
                                     npy_intp *steps, void *data)
{
    void *context_handle = geos_context[0];
    void *shell;
    char *ip1 = args[0], *ip2 = args[1], *op1 = args[2], *cp1;
    npy_intp is1 = steps[0], is2 = steps[1], os1 = steps[2], cs1 = steps[3];
    npy_intp n = dimensions[0], n_c1 = dimensions[1];
    npy_intp i, i_c1;
    for(i = 0; i < n; i++, ip1 += is1, ip2 += is2, op1 += os1) {
        GeometryObject *g = *(GeometryObject **)ip1;
        CHECK_GEOM(g);
        shell = GEOSGeom_clone_r(context_handle, g->ptr);
        if (shell == NULL) {
            return;
        }
        void** holes[n_c1];
        cp1 = ip2;
        for(i_c1 = 0; i_c1 < n_c1; i_c1++, cp1 += cs1) {
            GeometryObject *g = *(GeometryObject **)cp1;
            CHECK_GEOM(g);
            void *hole = GEOSGeom_clone_r(context_handle, g->ptr);
            if (hole == NULL) {
                return;
            }
            holes[i_c1] = hole;
        }
        GEOSGeometry *ret_ptr = GEOSGeom_createPolygon_r(context_handle, shell, holes, n_c1);
        OUTPUT_Y;
    }
}
static PyUFuncGenericFunction polygons_with_holes_funcs[1] = {&polygons_with_holes_func};

/*
TODO custom buffer functions
TODO possibly implement some creation functions
TODO polygonizer functions
TODO prepared geometry predicate functions
TODO relate functions
TODO G -> char function GEOSisValidReason_r
TODO Gi -> void function GEOSSetSRID_r
TODO G -> void function GEOSNormalize_r
TODO GGd -> d function GEOSHausdorffDistanceDensify_r

*/


#define DEFINE_Y_b(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Y_b_funcs, NAME ##_data, Y_b_dtypes, 1, 1, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_YY_b(NAME)\
    ufunc = PyUFunc_FromFuncAndData(YY_b_funcs, NAME ##_data, YY_b_dtypes, 1, 2, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_Y_Y(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Y_Y_funcs, NAME ##_data, Y_Y_dtypes, 1, 1, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_Yi_Y(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Yi_Y_funcs, NAME ##_data, Yi_Y_dtypes, 1, 2, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_Yd_Y(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Yd_Y_funcs, NAME ##_data, Yd_Y_dtypes, 1, 2, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_YY_Y(NAME)\
    ufunc = PyUFunc_FromFuncAndData(YY_Y_funcs, NAME ##_data, YY_Y_dtypes, 1, 2, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_Y_d(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Y_d_funcs, NAME ##_data, Y_d_dtypes, 1, 1, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_Y_B(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Y_B_funcs, NAME ##_data, Y_B_dtypes, 1, 1, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_Y_i(NAME)\
    ufunc = PyUFunc_FromFuncAndData(Y_i_funcs, NAME ##_data, Y_i_dtypes, 1, 1, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_YY_d(NAME)\
    ufunc = PyUFunc_FromFuncAndData(YY_d_funcs, NAME ##_data, YY_d_dtypes, 1, 2, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_YY_d_2(NAME)\
    ufunc = PyUFunc_FromFuncAndData(YY_d_2_funcs, NAME ##_data, YY_d_2_dtypes, 1, 2, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_CUSTOM(NAME, N_IN)\
    ufunc = PyUFunc_FromFuncAndData(NAME ##_funcs, null_data, NAME ##_dtypes, 1, N_IN, 1, PyUFunc_None, # NAME, "", 0);\
    PyDict_SetItemString(d, # NAME, ufunc)

#define DEFINE_GENERALIZED(NAME, N_IN, SIGNATURE)\
    ufunc = PyUFunc_FromFuncAndDataAndSignature(NAME ##_funcs, null_data, NAME ##_dtypes, 1, N_IN, 1, PyUFunc_None, # NAME, "", 0, SIGNATURE);\
    PyDict_SetItemString(d, # NAME, ufunc)


PyMODINIT_FUNC PyInit_ufuncs(void)
{
    PyObject *m, *d, *ufunc;
    m = PyModule_Create(&moduledef);
    if (!m) {
        return NULL;
    }
    if (PyType_Ready(&GeometryType) < 0)
        return NULL;

    Py_INCREF(&GeometryType);
    PyModule_AddObject(m, "GEOSGeometry", (PyObject *) &GeometryType);

    d = PyModule_GetDict(m);

    import_array();
    import_umath();

    void *context_handle = GEOS_init_r();
    PyObject* GEOSException = PyErr_NewException("pygeos.GEOSException", NULL, NULL);
    PyModule_AddObject(m, "GEOSException", GEOSException);
    GEOSContext_setErrorMessageHandler_r(context_handle, HandleGEOSError, GEOSException);
    GEOSContext_setNoticeMessageHandler_r(context_handle, HandleGEOSNotice, NULL);
    geos_context[0] = context_handle;  /* for global access */

    DEFINE_Y_b (is_empty);
    DEFINE_Y_b (is_simple);
    DEFINE_Y_b (is_ring);
    DEFINE_Y_b (has_z);
    DEFINE_Y_b (is_closed);
    DEFINE_Y_b (is_valid);

    DEFINE_YY_b (disjoint);
    DEFINE_YY_b (touches);
    DEFINE_YY_b (intersects);
    DEFINE_YY_b (crosses);
    DEFINE_YY_b (within);
    DEFINE_YY_b (contains);
    DEFINE_YY_b (overlaps);
    DEFINE_YY_b (equals);
    DEFINE_YY_b (covers);
    DEFINE_YY_b (covered_by);

    DEFINE_Y_Y (clone);
    DEFINE_Y_Y (envelope);
    DEFINE_Y_Y (convex_hull);
    DEFINE_Y_Y (boundary);
    DEFINE_Y_Y (unary_union);
    DEFINE_Y_Y (point_on_surface);
    DEFINE_Y_Y (get_centroid);
    DEFINE_Y_Y (line_merge);
    DEFINE_Y_Y (extract_unique_points);
    DEFINE_Y_Y (get_start_point);
    DEFINE_Y_Y (get_end_point);
    DEFINE_Y_Y (get_exterior_ring);
    DEFINE_Y_Y (normalize);

    DEFINE_Yi_Y (get_interior_ring_n);
    DEFINE_Yi_Y (get_point_n);
    DEFINE_Yi_Y (get_geometry_n);

    DEFINE_Yd_Y (interpolate);
    DEFINE_Yd_Y (interpolate_normalized);
    DEFINE_Yd_Y (simplify);
    DEFINE_Yd_Y (topology_preserve_simplify);

    DEFINE_YY_Y (intersection);
    DEFINE_YY_Y (difference);
    DEFINE_YY_Y (symmetric_difference);
    DEFINE_YY_Y (union);
    DEFINE_YY_Y (shared_paths);

    DEFINE_Y_d (get_x);
    DEFINE_Y_d (get_y);
    DEFINE_Y_d (area);
    DEFINE_Y_d (length);
    DEFINE_Y_d (get_length);

    DEFINE_Y_B (geom_type_id);
    DEFINE_Y_B (get_dimensions);
    DEFINE_Y_B (get_coordinate_dimensions);

    DEFINE_Y_i (get_srid);
    DEFINE_Y_i (get_num_geometries);
    DEFINE_Y_i (get_num_interior_rings);
    DEFINE_Y_i (get_num_points);
    DEFINE_Y_i (get_num_coordinates);

    DEFINE_YY_d (distance);
    DEFINE_YY_d (hausdorff_distance);

    DEFINE_YY_d_2 (project);
    DEFINE_YY_d_2 (project_normalized);

    DEFINE_CUSTOM (buffer, 3);
    DEFINE_CUSTOM (snap, 3);
    DEFINE_CUSTOM (equals_exact, 3);
    DEFINE_GENERALIZED(points, 1, "(d)->()");
    DEFINE_GENERALIZED(linestrings, 1, "(i, d)->()");
    DEFINE_GENERALIZED(linearrings, 1, "(i, d)->()");
    DEFINE_Y_Y (polygons_without_holes);
    DEFINE_GENERALIZED(polygons_with_holes, 2, "(),(i)->()");

    Py_DECREF(ufunc);
    return m;
}
