package com.mwine.bridge;

import android.app.Activity;
import android.os.Bundle;

/**
 * 无界面解冻入口：新装/被换装的应用处于 stopped 态，隐式广播不投递。
 * 宿主在广播导出壁纸前 `am start -n com.mwine.bridge/.UnlockActivity` 一次即可。
 */
public class UnlockActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        finish();
    }
}
