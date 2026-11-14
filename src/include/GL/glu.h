#ifndef IPO_GLU_H
#define IPO_GLU_H

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
void gluLookAt(GLdouble eyex, GLdouble eyey, GLdouble eyez,
               GLdouble centerx, GLdouble centery, GLdouble centerz,
               GLdouble upx, GLdouble upy, GLdouble upz);
void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top);

#ifdef __cplusplus
}
#endif

#endif /* IPO_GLU_H */

