-- EXPLICITLY SYNTHETIC disposable legacy MODEL baseline fixture only.
-- The real report reader requires an explicit disabled governance capability.
-- No users, grants, decisions, receipts or investor release are fabricated.
INSERT INTO trading.qt_workflow_capabilities(book_id,enabled,version) VALUES
 ('BASE_PORTFOLIO',false,1),
 ('CONSERVATIVE_PORTFOLIO',false,1);
