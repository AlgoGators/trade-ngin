"""The daily book e-mail: the engine's report laid out in the fund's design, then sent.

The engine (live_portfolio, QT_MAIL_BACKEND=python) builds the report as it always has and runs
`python -m algogators_rpc.services.report --portfolio-id ID --date YYYY-MM-DD --manifest FILE`.
The manifest carries the subject, the HTML body, the charts and the CSV files; this package reads
the body into blocks (engine_report), lays them out (renderer, templates/daily_report.html) and
sends the message (mailer). It computes no figure of its own.
"""
