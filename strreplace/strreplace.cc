/*
 * git-replace / replace string in git history
 * Copyright (C) 2015, Chhatoi Pritam Baral <pritam@pritambaral.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string>
#include <pcrecpp.h>
#include "strreplace.hh"

static pcrecpp::RE *pattern = NULL;
static pcrecpp::StringPiece replacement;
static std::string replaced;

void set_regex(const char *input_pat, const char *input_rep)
{
	pattern = new pcrecpp::RE(input_pat);
	replacement = pcrecpp::StringPiece(input_rep);
}

int replace(const char *c_str, char **dest) {
	replaced = std::string(c_str);
	int count = pattern->GlobalReplace(replacement, &replaced);
	*dest = (char *) malloc(sizeof(char) * (replaced.length() + 1));
	strncpy(*dest, replaced.c_str(), replaced.length());
	(*dest)[replaced.length()] = '\0';
	return count;
}
