import pandas as pd
import numpy as np
import databento as db
from datetime import datetime

def request_continuous_daily_data(symbols: list[str], dataset: db.Dataset, start_date: str | datetime | pd.Timestamp, end_date: str | datetime | pd.Timestamp, client: db.Historical = db.Historical()) -> pd.DataFrame:
    """Requesting daily OHLCV data and reference data for a list of symbols from Databento.

    Args:
        symbols (list[str]): List of each symbol to request data for | e.g. ["ES.c.0", "NQ.c.0"] signifies ES and NQ continuous rolled based on the calendar expiration and the 0th contract.
        start_date (str | datetime | pd.Timestamp): The start date of the data to request.
        end_date (str | datetime | pd.Timestamp): The end date of the data to request.

    Returns:
        pd.DataFrame: A DataFrame with the requested data including OHLCV, contract ID, symbol, and daily expirations.
    """
    # Begin request for daily data
    try:
        data = client.timeseries.get_range(
            dataset=dataset,
            start=start_date,
            end=end_date,
            symbols=symbols,
            schema=db.Schema.OHLCV_1D,
            stype_in=db.SType.CONTINUOUS,
            stype_out=db.SType.INSTRUMENT_ID,
        )
    except Exception as e:
        raise ValueError(f"Error requesting data: {e}")
    
    # Request reference data using DBNStore method
    ref = data.request_full_definitions(client=client)
    
    data_as_df = data.to_df()
    ref_as_df = ref.to_df()
    
    # Turn the non-daily expirations in reference data to daily