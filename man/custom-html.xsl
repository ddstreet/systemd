<?xml version='1.0'?> <!--*-nxml-*-->

<!--
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
-->

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/html/docbook.xsl"/>

<!-- translate man page references to links to html pages -->
<xsl:template match="citerefentry">
  <a>
    <xsl:attribute name="href">
      <xsl:value-of select="refentrytitle"/><xsl:text>.html</xsl:text>
    </xsl:attribute>
    <xsl:call-template name="inline.charseq"/>
  </a>
</xsl:template>

<!-- add Index link at top of page -->
<xsl:template name="user.header.content">
  <a>
    <xsl:attribute name="href">
      <xsl:text>index.html</xsl:text>
    </xsl:attribute>
    <xsl:text>Index </xsl:text>
  </a>·
  <a>
    <xsl:attribute name="href">
      <xsl:text>systemd.directives.html</xsl:text>
    </xsl:attribute>
    <xsl:text>Directives </xsl:text>
  </a>·
  <a>
    <xsl:attribute name="href">
      <xsl:text>../python-systemd/index.html</xsl:text>
    </xsl:attribute>
    <xsl:text>Python </xsl:text>
  </a>·
  <a>
    <xsl:attribute name="href">
      <xsl:text>../libudev/index.html</xsl:text>
    </xsl:attribute>
    <xsl:text>libudev </xsl:text>
  </a>·
  <a>
    <xsl:attribute name="href">
      <xsl:text>../libudev/index.html</xsl:text>
    </xsl:attribute>
    <xsl:text>gudev </xsl:text>
  </a>

  <span style="float:right">
    <xsl:text>systemd </xsl:text>
    <xsl:value-of select="$systemd.version"/>
  </span>
  <hr/>
</xsl:template>

<!-- Switch things to UTF-8, ISO-8859-1 is soo yesteryear -->
<xsl:output method="html" encoding="UTF-8" indent="no"/>

</xsl:stylesheet>
