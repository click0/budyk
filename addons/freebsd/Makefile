# SPDX-License-Identifier: BSD-3-Clause

PORTNAME=	budyk
DISTVERSIONPREFIX=	v
DISTVERSION=	0.8.3
CATEGORIES=	sysutils

MAINTAINER=	ports@FreeBSD.org
COMMENT=	Lightweight server monitoring with adaptive collection
WWW=		https://github.com/click0/budyk

LICENSE=	BSD3CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

LIB_DEPENDS=	libargon2.so:security/libargon2 \
		libyaml.so:textproc/libyaml

USES=		cmake lua:5.4 pkgconfig
USE_GITHUB=	yes
GH_ACCOUNT=	click0
USE_RC_SUBR=	budyk

CMAKE_OFF=	BUILD_TESTS ENABLE_LLM STATIC_LINK

# Not in the ports tree's UIDs and GIDs yet: a port submitted to it adds
# them there (CI adds them to its copy of the tree).
USERS=		budyk
GROUPS=		budyk

# The config and the rules as samples (pkg copies each to its name
# without .sample, and keeps one that was changed), the rules the
# FreeBSD defaults: the config's rules.path names that file.
post-install:
	${MV} ${STAGEDIR}${PREFIX}/etc/budyk/config.example.yaml \
		${STAGEDIR}${PREFIX}/etc/budyk/config.yaml.sample
	${INSTALL_DATA} ${WRKSRC}/rules/freebsd-defaults.lua \
		${STAGEDIR}${PREFIX}/etc/budyk/rules.lua.sample
	${MKDIR} ${STAGEDIR}/var/db/budyk

.include <bsd.port.mk>
