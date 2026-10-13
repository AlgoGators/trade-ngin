#!/usr/bin/env python3
"""scripts/live_mode_gate/compare_bodies.py: two report bodies of the same date, with the chart rule.

    compare_bodies.py <body A> <log A (.gz)> <body B> <log B (.gz)>

A chart process that does not finish in time is killed, the run goes on and its report leaves that
one chart block out (one ERROR line in the run's log: "Chart not drawn after 2 start(s): ...", and one line in its place in the report: "<chart>: not shown (<reason>)"). On a
loaded machine that happens to about one chart process in a hundred, on either side, so two runs
that did the same thing can differ by a chart block. The rule:

  * neither log holds a chart timeout line: the two bodies must be BYTE-identical;
  * otherwise: the bodies must be identical once every chart block is taken out of both, AND each
    side may lack, of the charts the other side has, at most as many as its own log has timeout
    lines. A chart missing with no timeout line to account for it fails; so does any difference
    that is not a chart block.

The blocks are compared out rather than the run made again: a second run of the date would take the
run lock again and re-write the day's rows, and the comparison would then depend on which build of
the body was kept.

exit 0 identical under the rule, 1 different, 2 usage. One line says which arm ruled.
"""
import gzip
import re
import sys

TIMEOUT = "Chart not drawn after"   # the one ERROR line of a chart left out after its second start (core/gnuplot_run.hpp)
# The line the report prints in the place of a chart that is not shown (core/report_charts.hpp).
NOT_SHOWN = re.compile(r'<p style="margin: 12px 0; color: #666; font-size: 13px;">[^<\n]*: not shown \([^)\n]*\)</p>\n')
# One chart block as the report writes it: heading, wrapper, image, wrapper end.
BLOCK = re.compile(
    r'<h3 style="margin-top: 20px; color: #333;">[^<\n]*</h3>\n'
    r'<div style="[^"\n]*">\n'
    r'<img src="cid:([a-z_0-9]+)"[^\n]*/>\n'
    r'</div>\n')


def read_log(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", errors="replace") as f:
        return f.read()


def main(argv):
    if len(argv) != 5:
        print(__doc__)
        return 2
    body_a, log_a, body_b, log_b = argv[1:5]
    a = open(body_a, encoding="utf-8", errors="replace").read()
    b = open(body_b, encoding="utf-8", errors="replace").read()
    killed_a = read_log(log_a).count(TIMEOUT)
    killed_b = read_log(log_b).count(TIMEOUT)
    if killed_a == 0 and killed_b == 0:
        if open(body_a, "rb").read() == open(body_b, "rb").read():
            print("IDENTICAL byte for byte (no chart timeout in either log)")
            return 0
        print("DIFFERENT: the bodies differ and neither log holds a chart timeout line")
        return 1
    charts_a, charts_b = BLOCK.findall(a), BLOCK.findall(b)
    lacks_a = [c for c in charts_b if c not in charts_a]
    lacks_b = [c for c in charts_a if c not in charts_b]
    if NOT_SHOWN.sub("", BLOCK.sub("", a)) != NOT_SHOWN.sub("", BLOCK.sub("", b)):
        print("DIFFERENT outside the chart blocks (chart timeouts: A %d, B %d)" % (killed_a, killed_b))
        return 1
    if len(lacks_a) > killed_a or len(lacks_b) > killed_b:
        print("DIFFERENT: a chart is missing with no timeout line to account for it "
              "(A lacks %s with %d timeout line(s); B lacks %s with %d)"
              % (lacks_a or "none", killed_a, lacks_b or "none", killed_b))
        return 1
    print("IDENTICAL but for killed charts (A lacks %s, %d timeout line(s); B lacks %s, %d)"
          % (lacks_a or "none", killed_a, lacks_b or "none", killed_b))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
