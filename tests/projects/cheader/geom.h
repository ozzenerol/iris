#define GEOM_LIMIT 10
#define GEOM_MASK 0x0Fu
#define GEOM_NAME "geom"

enum shape_kind { SHAPE_CIRCLE, SHAPE_RECT = 5, SHAPE_TRI };

struct point {
    int x;
    int y;
};

typedef struct {
    struct point from;
    struct point to;
    unsigned char kind;
} segment;

typedef unsigned long long big;

double area(double r);
