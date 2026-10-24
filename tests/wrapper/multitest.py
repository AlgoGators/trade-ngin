from tradengin import BacktestRunner

from trend_following import TrendFollowingStrategy

from simple_trend import SimpleTrendStrategy


def main():
    bt = BacktestRunner()
    bt.initialize("multi_test")

    # bt.register_trend_following_strategy()
    bt.register_strategy("TREND_FOLLOWING", TrendFollowingStrategy)
    bt.register_strategy("SIMPLE_TREND", SimpleTrendStrategy)

    result = bt.run_backtest()
    print(result)


if __name__ == "__main__":
    main()
