#include <stdint.h>
#include <stdbool.h>

typedef enum strela_err strela_err;
enum strela_err {
	STRELA_ERR_OK,
	STRELA_ERR_ARG,
};

/* STRELA result type. Negative numbers are used for STRELA specific errors
 * while positive ones shall be interpreted as classic errno(3) errors.
 */
typedef struct strela_res strela_res;
struct strela_res { int errnum; };

inline bool strela_res_ok(strela_res res) { return res.errnum == STRELA_ERR_OK; }

/* STRELA per device context.
 */
typedef struct strela_ctx strela_ctx;
struct strela_ctx {
	int fd;
	uint32_t *base;

	strela_res res;
};
