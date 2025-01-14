import databento as db
from trade_ngin.adapters.databento import request_continuous_daily_data
from datetime import datetime, timedelta
import os

# Set the API key in environment
os.environ["DATABENTO_API_KEY"] = "db-7MLnkmd4uLXbsy6MshdB9jRjivG8"

# Create client with API key
client = db.Historical(key=os.environ["DATABENTO_API_KEY"])

# Request just 5 days of data for MES to minimize cost
end_date = datetime.now()
start_date = end_date - timedelta(days=5)

try:
    # Request data for MES continuous front month
    data = request_continuous_daily_data(
        symbols=["MES.c.0"],
        dataset=db.Dataset.GLBX,  # CME Globex
        start_date=start_date,
        end_date=end_date,
        client=client
    )
    
    print(f"Successfully retrieved {len(data)} rows of data")
    print("\nFirst few rows:")
    print(data.head())
    
except Exception as e:
    print(f"Error retrieving data: {e}") 