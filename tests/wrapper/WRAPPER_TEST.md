To test, create a config according to the instructions provided in the main folder, and add a "multi_test" portfolio with the `portfolio.json` file (see other portfolios for reference). Then, run the following command:

```bash
uv run multitest.py
```

You can also test the conservative portfolio by running:

```bash
uv run conservative.py
```

The multi_test portfolio succeeded in running a backtest over 2 strategies with different asset classes, and the conservative portfolio succeeded in matching the expected results for the C++ implementation.
