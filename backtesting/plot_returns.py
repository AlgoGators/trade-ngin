import pandas as pd
import matplotlib.pyplot as plt

def plot_cumulative_returns(csv_file):
    data = pd.read_csv(csv_file)
    plt.figure(figsize=(10, 6))
    plt.plot(data['Date'], data['Cumulative Return (%)'], label='Cumulative Return (%)', color='blue')
    plt.title('Cumulative Daily Returns')
    plt.xlabel('Date')
    plt.ylabel('Cumulative Return (%)')
    plt.xticks(rotation=45)
    plt.grid()
    plt.legend()
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    plot_cumulative_returns("cumulative_returns.csv")
