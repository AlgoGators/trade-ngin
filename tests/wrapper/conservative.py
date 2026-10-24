from tradengin import BacktestRunner

from trend_following import TrendFollowingStrategy

# from simple_trend import SimpleTrendStrategy


def main():
    bt = BacktestRunner()
    bt.initialize("conservative")

    # bt.register_trend_following_strategy()
    bt.register_strategy("TREND_FOLLOWING", TrendFollowingStrategy)

    result = bt.run_backtest()
    print(result)


if __name__ == "__main__":
    main()
