import numpy as np

def process_ohlcv(ohlcv):
    """
    Process OHLCV data (C++ -> Python)
    :param ohlcv: List of lists (OHLCV data)
    :return: None (Prints statistics)
    """
    data = np.array(ohlcv) 
    avg_close = np.mean(data[:, 3]) 
    total_volume = np.sum(data[:, 4]) 
    print(f"Data: {data}")
