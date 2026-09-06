'use strict';

import { glob, basename, popen } from "fs";

export const TYPE_ARRAY = 1;
export const TYPE_STRING = 3;
export const TYPE_INT = 5;
export const TYPE_BOOL = 7;

export function parse_bool(val)
{
	switch (val) {
	case "1":
	case "true":
		return true;
	case "0":
	case "false":
		return false;
	}
};

export function parse_array(val)
{
	if (type(val) != "array")
		val = split(val, /\s+/);
	return val;
};

function __type_parsers()
{
	let ret = [];

	ret[TYPE_ARRAY] = parse_array;
	ret[TYPE_STRING] = function(val) {
		return val;
	};
	ret[TYPE_INT] = function(val) {
		return +val;
	};
	ret[TYPE_BOOL] = parse_bool;

	return ret;
}
export const type_parser = __type_parsers();

export function handler_load(path, cb)
{
	for (let script in glob(path + "/*.sh")) {
		script = basename(script);

		/*
		 * ★ READ THE HANDLER'S "dump" THROUGH popen(), NOT through a
		 * mkstemp() temp file that the child writes to via an inherited fd.
		 *
		 * Upstream does:
		 *     let f = mkstemp();
		 *     chdir(path);
		 *     system(`./${script} "" "dump" >&${f.fileno()}`);
		 *     chdir(prev_dir);
		 *     f.seek();
		 *     while (!f.error()) { ... f.read("line") ... }
		 *
		 * On this target that loses every handler after the FIRST one. Proven
		 * on hardware 2026-09-03 by instrumenting a copy of the shipped
		 * /lib/netifd/utils.uc: with mac80211.sh + rtl8192cd.sh present, the
		 * callback fires once (mac80211) and rtl8192cd is silently dropped;
		 * add a third handler that sorts first and it is the SECOND one that
		 * disappears, whichever script that happens to be. The child always
		 * exits 0 and the bytes really are in the temp file -- an injected
		 * debug read right after f.seek() returns the full 757-byte dump --
		 * but the loop's own first f.read("line") then comes back empty, so it
		 * breaks immediately. An explicit f.seek(0) does not help. The parent's
		 * buffered FILE* and the child's raw writes through the inherited fd
		 * disagree about the offset, and reusing the same fd number across
		 * iterations makes it deterministic from the second handler onward.
		 *
		 * The consequence is severe and completely silent: netifd's
		 * config_init() does `let handler = wireless.handlers[data.type]; if
		 * (!handler) continue;` (wifi-scripts wireless.uc), so an unloaded
		 * handler means the radio is skipped with no log line at all --
		 * `ubus call network.wireless status` simply never lists it and
		 * `wifi up <radio>` answers "Not found". That is exactly how the vendor
		 * 2.4 GHz radio1 vanished on this port while its handler sat installed
		 * and executable in /lib/netifd/wireless/ and dumped valid JSON by hand.
		 *
		 * popen() streams the child's stdout directly: no temp file, no fd
		 * redirect, no seek, and no shared-offset ambiguity. `cd` inside the
		 * command preserves the handler's expectation of running with the
		 * handler directory as its cwd, without mutating the parent's cwd the
		 * way the chdir()/chdir() pair did.
		 */
		let f = popen(`cd '${path}' && ./'${script}' "" "dump"`, "r");
		if (!f)
			continue;

		for (let line = f.read("line"); length(line); line = f.read("line")) {
			let data = trim(line);
			try {
				data = json(data);
			} catch (e) {
				continue;
			}

			if (type(data) != "object")
				continue;

			cb(script, data);
		}
		f.close();
	}
};

export function handler_attributes(data, extra, validate)
{
	let ret = { ...extra };
	for (let cur in data) {
		let name_data = split(cur[0], ":", 2);
		let name = name_data[0];
		ret[name] = cur[1];
		if (validate && name_data[1])
			validate[name] = name_data[1];
	}
	return ret;
};

export function parse_attribute_list(data, spec)
{
	let ret = {};

	for (let name, type_id in spec) {
		if (!(name in data))
			continue;

		let val = data[name];
		let parser = type_parser[type_id];
		if (parser)
			val = parser(val);
		ret[name] = val;
	}

	return ret;
};

export function sorted_json(value) {
	let t = type(value);

	if (t == "object") {
		let parts = [];
		for (let key in sort(keys(value)))
			push(parts, sprintf("%J", key) + ":" + sorted_json(value[key]));
		return "{" + join(",", parts) + "}";
	}

	if (t == "array") {
		let parts = [];
		for (let item in value)
			push(parts, sorted_json(item));
		return "[" + join(",", parts) + "]";
	}

	return sprintf("%J", value);
};

export function is_equal(val1, val2) {
	let t1 = type(val1);

	if (t1 != type(val2))
		return false;

	if (t1 == "array") {
		if (length(val1) != length(val2))
			return false;

		for (let i = 0; i < length(val1); i++)
			if (!is_equal(val1[i], val2[i]))
				return false;

		return true;
	} else if (t1 == "object") {
		for (let key in val1)
			if (!is_equal(val1[key], val2[key]))
				return false;
		for (let key in val2)
			if (val1[key] == null)
				return false;
		return true;
	} else {
		return val1 == val2;
	}
};
