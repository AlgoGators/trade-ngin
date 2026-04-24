import pandas as pd
from sqlalchemy import create_engine
import yfinance as yf

database = {
    "host": "13.58.153.216",
    "port": "5432",
    "user": "postgres",
    "password": "algogators",
    "dbname": "new_algo_data"
}
engine = create_engine(
    f"postgresql+psycopg://{database['user']}:{database['password']}@{database['host']}:{database['port']}/{database['dbname']}"
)

class DataFetcher:
    def __init__(self):
        engine = create_engine(
            f"postgresql+psycopg://{database['user']}:{database['password']}@{database['host']}:{database['port']}/{database['dbname']}"
        )
        self.engine = engine

    def fetch_data(self, symbol, limit=1000):
        query = f"""
        SELECT *
        FROM futures_data.new_data_ohlcv_1d
        WHERE symbol = '{symbol}'
        ORDER BY "time" DESC
        LIMIT {limit};
        """
        df = pd.read_sql(query, self.engine)
        return df

    def fetch_global_macro_data(self, tables=None, limit=1000):
        if tables is None:
            tables = [
                "credit_spreads",
                "growth",
                "inflation",
                "liquidity",
                "market",
                "yield_curve",
            ]

        frames = []
        for table in tables:
            query = f"""
            SELECT *
            FROM macro_data.{table}
            ORDER BY date DESC
            LIMIT {limit};
            """
            df = pd.read_sql(query, self.engine)
            df = df.rename(columns={"date": "time"})
            # Prefix columns with table name to avoid collisions (e.g. inflation.cpi)
            df = df.rename(columns={c: f"{table}_{c}" for c in df.columns if c != "time"})
            frames.append(df)

        # Merge all tables on time
        combined = frames[0]
        for df in frames[1:]:
            combined = combined.merge(df, on="time", how="outer")

        combined["time"] = pd.to_datetime(combined["time"], utc=True)
        return combined.sort_values("time").reset_index(drop=True)

    """
    * VIX for implied vol (yfinance)
    * treasury yield spreads (2yr, 5yr, 10yr - database)
    * unemployment (yfinance)
    """
    def fetch_macro_data(self, limit=1000):


        # Futures closes (ZT, ZF) -> wide
        q = f"""
        SELECT "time", symbol, close
        FROM futures_data.new_data_ohlcv_1d
        WHERE symbol IN ('ZT', 'ZF')
        ORDER BY "time" DESC
        LIMIT {limit};
        """
        fut = pd.read_sql(q, self.engine)

        fut["time"] = pd.to_datetime(fut["time"], utc=True)
        fut["date"] = fut["time"].dt.floor("D")

        # If multiple rows per (date, symbol), keep the latest timestamp
        fut = fut.sort_values("time").drop_duplicates(subset=["date", "symbol"], keep="last")

        # Pivot to columns
        fut_wide = (
            fut.pivot(index="date", columns="symbol", values="close")
            .reset_index()
            .rename(columns={"date": "time", "ZT": "treas_2y_close", "ZF": "treas_5y_close"})
        )

        # VIX close (yfinance)
        vix_raw = yf.download("^VIX", period="max", interval="1d", auto_adjust=False)["Close"]

        # Handle MultiIndex columns (yfinance >= 0.2.x returns DataFrame with ticker level)
        if isinstance(vix_raw, pd.DataFrame):
            vix_raw = vix_raw.iloc[:, 0]  # squeeze to Series

        vix = vix_raw.rename("vix_close").to_frame().reset_index()

        # fix yfinance Date vs Datetime
        if "Date" in vix.columns:
            vix = vix.rename(columns={"Date": "time"})
        elif "Datetime" in vix.columns:
            vix = vix.rename(columns={"Datetime": "time"})
        else:
            # fallback: first column is the date-like column
            vix = vix.rename(columns={vix.columns[0]: "time"})

        vix["time"] = pd.to_datetime(vix["time"], utc=True).dt.floor("D")

        # Merge side-by-side
        df = fut_wide.merge(vix, on="time", how="left").sort_values("time").reset_index(drop=True)

        # keep only dates where all 3 exist
        df = df.dropna(subset=["treas_2y_close", "treas_5y_close", "vix_close"]).reset_index(drop=True)

        return df


if __name__ == "__main__":
    df = DataFetcher().fetch_data('NG', limit=100)
    print(df.head())
    df_macro = DataFetcher().fetch_macro_data(limit=100)
    print(df_macro.head())
    print(f"Fetched {len(df)} rows of futures data and {len(df_macro)} rows of macro data")
    df_global = DataFetcher().fetch_global_macro_data(limit=100)
    print(df_global.head())
    print(f"Fetched {len(df_global)} rows of global macro data")