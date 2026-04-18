# SPDX-License-Identifier: BSD-3-Clause

PORTNAME=       budyk
DISTVERSIONPREFIX= v
DISTVERSION=    0.1.0
CATEGORIES=     sysutils

MAINTAINER=     ports@FreeBSD.org
COMMENT=        Lightweight server monitoring with adaptive collection
WWW=            https://github.com/click0/budyk

LICENSE=        BSD3CLAUSE
LICENSE_FILE=   ${WRKSRC}/LICENSE

USES=           cmake pkgconfig lua:5.4

LIB_DEPENDS=    libargon2.so:security/libargon2 \
	        libyaml.so:textproc/libyaml

USE_GITHUB=     yes
GH_ACCOUNT=     click0
GH_PROJECT=     budyk

USE_RC_SUBR=    budyk

CMAKE_ARGS=     -DSTATIC_LINK=OFF -DENABLE_LLM=OFF -DBUDYK_PLATFORM=freebsd

USERS=          budyk
GROUPS=         budyk

PLIST_SUB=      BUDYK_USER=${USERS} BUDYK_GROUP=${GROUPS}

post-install:
	${MKDIR} ${STAGEDIR}${PREFIX}/etc/budyk
	${INSTALL_DATA} ${WRKSRC}/config.example.yaml \
	        ${STAGEDIR}${PREFIX}/etc/budyk/config.example.yaml

.include <bsd.port.mk>
