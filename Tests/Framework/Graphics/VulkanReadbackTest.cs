// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

#if VULKAN
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using NUnit.Framework;

namespace MonoGame.Tests.Graphics
{
    [NonParallelizable]
    [RunOnUiTestFixture]
    public class VulkanReadbackTest
    {
        [TestCase(0)]
        [TestCase(1)]
        [TestCase(3)]
        public void ReadbackPreservesPresentationAndResize(int readsPerFrame)
        {
            using var game = new TestGameBase();
            using var manager = new GraphicsDeviceManager(game)
            {
                PreferredBackBufferWidth = 800,
                PreferredBackBufferHeight = 600,
                GraphicsProfile = GraphicsProfile.HiDef
            };
            manager.ApplyChanges();
            var device = manager.GraphicsDevice;
            using var batch = new SpriteBatch(device);
            using var pixel = new Texture2D(device, 1, 1);
            pixel.SetData(new[] { Color.White });

            for (var frame = 0; frame < 9; frame++)
            {
                if (frame == 3 || frame == 6)
                {
                    manager.PreferredBackBufferWidth = frame == 3 ? 640 : 800;
                    manager.PreferredBackBufferHeight = frame == 3 ? 512 : 600;
                    manager.ApplyChanges();
                }

                var width = manager.PreferredBackBufferWidth;
                var height = manager.PreferredBackBufferHeight;
                Assert.That(game.Window.ClientBounds.Size, Is.EqualTo(new Point(width, height)));
                device.Clear(Color.CornflowerBlue);
                batch.Begin(blendState: BlendState.Opaque);
                batch.Draw(pixel, new Rectangle(0, 0, 16, 16), Color.Red);
                batch.Draw(pixel, new Rectangle(width - 16, height - 16, 16, 16), Color.Lime);
                batch.End();

                for (var read = 0; read < readsPerFrame; read++)
                {
                    var data = new Color[width * height];
                    device.GetBackBufferData(data);
                    Assert.That(data[8 * width + 8], Is.EqualTo(Color.Red));
                    Assert.That(data[(height - 8) * width + width - 8], Is.EqualTo(Color.Lime));
                    Assert.That(data[(height / 2) * width + width / 2], Is.EqualTo(Color.CornflowerBlue));

                    // Resume drawing in the same frame after each synchronous readback.
                    batch.Begin(blendState: BlendState.Opaque);
                    batch.Draw(pixel, new Rectangle(width - 16, 0, 16, 16), Color.Yellow);
                    batch.End();
                    var corner = new Color[1];
                    device.GetBackBufferData(new Rectangle(width - 8, 8, 1, 1), corner, 0, 1);
                    Assert.That(corner[0], Is.EqualTo(Color.Yellow));
                }

                device.Present();
            }
        }

        [Test]
        public void ReadbackPreservesViewportAndScissor()
        {
            using var game = new TestGameBase();
            using var manager = new GraphicsDeviceManager(game);
            manager.ApplyChanges();
            var device = manager.GraphicsDevice;
            using var batch = new SpriteBatch(device);
            using var pixel = new Texture2D(device, 1, 1);
            using var rasterizer = new RasterizerState { ScissorTestEnable = true };
            pixel.SetData(new[] { Color.White });
            device.Clear(Color.Black);
            device.Viewport = new Viewport(100, 100, 200, 200);
            device.ScissorRectangle = new Rectangle(120, 120, 30, 30);
            var corner = new Color[1];
            device.GetBackBufferData(new Rectangle(0, 0, 1, 1), corner, 0, 1);
            batch.Begin(blendState: BlendState.Opaque, rasterizerState: rasterizer);
            batch.Draw(pixel, new Rectangle(0, 0, 200, 200), Color.Red);
            batch.End();
            device.GetBackBufferData(new Rectangle(130, 130, 1, 1), corner, 0, 1);
            Assert.That(corner[0], Is.EqualTo(Color.Red));
            device.GetBackBufferData(new Rectangle(110, 110, 1, 1), corner, 0, 1);
            Assert.That(corner[0], Is.EqualTo(Color.Black));
            device.Present();
        }

        [Test]
        public void OcclusionQueryReadbackAndPresentationShareAcquisition()
        {
            using var game = new TestGameBase();
            using var manager = new GraphicsDeviceManager(game) { GraphicsProfile = GraphicsProfile.HiDef };
            manager.ApplyChanges();
            var device = manager.GraphicsDevice;
            using var batch = new SpriteBatch(device);
            using var pixel = new Texture2D(device, 1, 1);
            using var query = new OcclusionQuery(device);
            pixel.SetData(new[] { Color.White });
            for (var frame = 0; frame < 3; frame++)
            {
                device.Clear(Color.Black);
                query.Begin();
                batch.Begin(blendState: BlendState.Opaque);
                batch.Draw(pixel, new Rectangle(0, 0, 32, 32), Color.Red);
                batch.End();
                query.End();
                Assert.That(query.IsComplete, Is.True);
                Assert.That(query.PixelCount, Is.GreaterThan(0));
                var corner = new Color[1];
                device.GetBackBufferData(new Rectangle(8, 8, 1, 1), corner, 0, 1);
                Assert.That(corner[0], Is.EqualTo(Color.Red));
                device.Clear(Color.Lime);
                device.GetBackBufferData(new Rectangle(8, 8, 1, 1), corner, 0, 1);
                Assert.That(corner[0], Is.EqualTo(Color.Lime));
                device.Present();
            }
        }

        [Test]
        public void TextureReadbackAndBackbufferReadbackShareAcquisition()
        {
            using var game = new TestGameBase();
            using var manager = new GraphicsDeviceManager(game);
            manager.ApplyChanges();
            var device = manager.GraphicsDevice;
            using var target = new RenderTarget2D(device, 16, 16);
            for (var frame = 0; frame < 6; frame++)
            {
                device.Clear(Color.Red);
                device.SetRenderTarget(target);
                device.Clear(Color.Lime);
                var textureData = new Color[256];
                target.GetData(textureData);
                Assert.That(textureData[0], Is.EqualTo(Color.Lime));
                device.Clear(Color.Blue);
                target.GetData(textureData);
                Assert.That(textureData[255], Is.EqualTo(Color.Blue));
                device.SetRenderTarget(null);
                device.Clear(Color.Red);
                var corner = new Color[1];
                device.GetBackBufferData(new Rectangle(0, 0, 1, 1), corner, 0, 1);
                Assert.That(corner[0], Is.EqualTo(Color.Red));
                device.Present();
            }
        }
    }
}
#endif
