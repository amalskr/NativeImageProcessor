package com.ceylonapz.nativeimageprocessor

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import com.ceylonapz.nativeimageprocessor.ui.theme.NativeImageProcessorTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            NativeImageProcessorTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    Greeting(
                        name = "Android",
                        modifier = Modifier.padding(innerPadding)
                    )
                }
            }
        }
    }
}

@Composable
fun Greeting(name: String, modifier: Modifier = Modifier) {

    val processor = remember {
        NativeImageProcessor()
    }

    Column() {
        Text(
            text = "Hello $name!",
            modifier = modifier
        )

        Button(
            onClick = {
                processor.sayHello()
            }
        ) {
            Text("Call C++")
        }
    }
}

@Preview(showBackground = true)
@Composable
fun GreetingPreview() {
    NativeImageProcessorTheme {
        Greeting("Android")
    }
}