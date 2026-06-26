/* contrib/earthdistance/earthdistance.c */

#include "postgres.h"

#include <math.h>

#include "utils/geo_decls.h"	/* for Point */

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not
 *
 * X/Open (XSI) 需要 <math.h> 提供 M_PI，但核心 POSIX 不提供
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PG_MODULE_MAGIC_EXT(
					.name = "earthdistance",
					.version = PG_VERSION
);

/* Earth's radius is in statute miles.
 *
 * 地球半径以法定英里为单位。
 */
static const double EARTH_RADIUS = 3958.747716;
static const double TWO_PI = 2.0 * M_PI;


/******************************************************
 *
 * degtorad - convert degrees to radians
 *
 * degtorad - 将度数转换为弧度
 *
 * arg: double, angle in degrees
 *
 * arg：双精度，角度（以度为单位）
 *
 * returns: double, same angle in radians
 *
 * 返回：双精度，相同的角度（以弧度表示）
 ******************************************************/

static double
degtorad(double degrees)
{
	return (degrees / 360.0) * TWO_PI;
}

/******************************************************
 *
 * geo_distance_internal - distance between points
 *
 * geo_distance_internal - 点之间的距离
 *
 * args:
 *	 a pair of points - for each point,
 *	   x-coordinate is longitude in degrees west of Greenwich
 *	   y-coordinate is latitude in degrees above equator
 *
 * args：一对点 - 对于每个点，x 坐标是格林威治以西的经度，以度为单位 y 坐标是赤道以上的纬度，以度为单位
 *
 * returns: double
 *	 distance between the points in miles on earth's surface
 *
 * 返回：地球表面上两点之间的距离（以英里为单位）的两倍
 ******************************************************/

static double
geo_distance_internal(Point *pt1, Point *pt2)
{
	double		long1,
				lat1,
				long2,
				lat2;
	double		longdiff;
	double		sino;

	/* convert degrees to radians
	 *
	 * 将度数转换为弧度
	 */

	long1 = degtorad(pt1->x);
	lat1 = degtorad(pt1->y);

	long2 = degtorad(pt2->x);
	lat2 = degtorad(pt2->y);

	/* compute difference in longitudes - want < 180 degrees
	 *
	 * 计算经度差 - 想要 < 180 度
	 */
	longdiff = fabs(long1 - long2);
	if (longdiff > M_PI)
		longdiff = TWO_PI - longdiff;

	sino = sqrt(sin(fabs(lat1 - lat2) / 2.) * sin(fabs(lat1 - lat2) / 2.) +
				cos(lat1) * cos(lat2) * sin(longdiff / 2.) * sin(longdiff / 2.));
	if (sino > 1.)
		sino = 1.;

	return 2. * EARTH_RADIUS * asin(sino);
}


/******************************************************
 *
 * geo_distance - distance between points
 *
 * geo_distance - 点之间的距离
 *
 * args:
 *	 a pair of points - for each point,
 *	   x-coordinate is longitude in degrees west of Greenwich
 *	   y-coordinate is latitude in degrees above equator
 *
 * args：一对点 - 对于每个点，x 坐标是格林威治以西的经度，以度为单位 y 坐标是赤道以上的纬度，以度为单位
 *
 * returns: float8
 *	 distance between the points in miles on earth's surface
 *
 * 返回：float8 地球表面上各点之间的距离（以英里为单位）
 ******************************************************/

PG_FUNCTION_INFO_V1(geo_distance);

Datum
geo_distance(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);
	float8		result;

	result = geo_distance_internal(pt1, pt2);
	PG_RETURN_FLOAT8(result);
}
