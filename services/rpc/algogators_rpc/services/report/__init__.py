"""The daily book e-mail: read a portfolio's day from the database, render it, send it.

Run as `python -m algogators_rpc.services.report --portfolio-id ID --date YYYY-MM-DD`. The
renderer, charts and template are the AlgoLens report's, ported here when the send moved to
trade-ngin.
"""
