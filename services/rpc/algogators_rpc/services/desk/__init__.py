"""The QT desk service: AlgoLens' commands to the trade-ngin engine (plan 3b).

proto/algogators/desk.proto; docs/design/desk-service.md. Registered with the shared server by
service.py.
"""

API = "desk"
SERVICE_NAME = "algogators.desk.DeskService"
# The full name before the shared layer (proto package algogators.qt.v1). Served by the same
# servicer, without the version check, for AlgoLens builds that predate the header. Remove it in
# the release after AlgoLens moves to algogators.desk.
LEGACY_SERVICE_NAME = "algogators.qt.v1.DeskService"
